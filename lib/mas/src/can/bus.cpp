#include "mas/can/bus.h"

#include "mas/can/core/md.h"
#include "mas/can/core/setting.h"

namespace mas {
namespace can {

namespace {

constexpr uint32_t kCmdPeriodUs = 10000;     // 指令 100 Hz
constexpr uint32_t kSyncPeriodUs = 50000;    // SYNC 20 Hz
constexpr uint32_t kEstopPeriodUs = 100000;  // E-stop 10 Hz（A8）
constexpr uint32_t kMdTimeoutUs = 200000;    // FB が途絶えたとみなす時間
constexpr uint32_t kPadTimeoutUs = 200000;   // A6
constexpr uint32_t kReqTimeoutUs = 50000;
constexpr uint32_t kReqSaveTimeoutUs = 300000;
constexpr uint8_t kReqRetries = 2;
constexpr uint8_t kClearRetries = 3;
constexpr float kTwoPi = 6.28318530718f;

constexpr uint8_t kItemCurrentLsb = 48;
constexpr uint8_t kItemSpeedLsb = 49;

bool elapsed(uint32_t now, uint32_t since, uint32_t period) {
    return (uint32_t)(now - since) >= period;
}

mas_can_md_cmd_t toCmd(detail::MdMode m) {
    switch (m) {
        case detail::MdMode::Current:
            return MAS_CAN_MD_CMD_CURRENT;
        case detail::MdMode::Speed:
            return MAS_CAN_MD_CMD_SPEED;
        case detail::MdMode::Position:
            return MAS_CAN_MD_CMD_POSITION;
        default:
            return MAS_CAN_MD_CMD_DUTY;
    }
}

}  // namespace

void Bus::begin(Port &port) {
    port_ = &port;
    uint32_t now = port_->micros();
    last_cmd_us_ = now - kCmdPeriodUs;
    last_sync_us_ = now - kSyncPeriodUs;
    started_ = true;
}

void Bus::update() {
    if (!started_) return;
    uint32_t now = port_->micros();

    for (auto &p : pad_) p.pressed = p.released = 0;
    receiveAll(now);

    for (auto &m : md_) {
        if (m.fb.online && elapsed(now, m.fb.receivedUs, kMdTimeoutUs)) m.fb.online = false;
    }
    for (auto &p : pad_) {
        if (p.seen && elapsed(now, p.last_us, kPadTimeoutUs)) {  // A6: 受信基板が落ちた
            float dz = p.deadzone;
            uint16_t released = p.released | p.buttons;
            p = detail::ControllerState();
            p.deadzone = dz;
            p.released = released;
        }
    }

    serviceEstop(now);
    if (elapsed(now, last_cmd_us_, kCmdPeriodUs)) {
        last_cmd_us_ = now;
        sendCommands();
    }
    if (sync_source_ && elapsed(now, last_sync_us_, kSyncPeriodUs)) {
        Frame f;
        uint32_t t = port_->micros();  // 渡す直前に取る（requirements.md §4.7）
        mas_can_sync_encode(t, &f);
        if (port_->send(f)) last_sync_us_ = now;
    }
    serviceSettings(now);
}

void Bus::receiveAll(uint32_t now) {
    Frame f;
    while (port_->receive(f)) {
        mas_can_kind_t kind;
        if (mas_can_is_estop(&f)) {
            estop_seen_ = true;
        } else if (mas_can_is_estop_clear(&f)) {
            estop_seen_ = false;
        } else if (mas_can_boot_decode(&f, &kind)) {
            if (kind == MAS_CAN_KIND_MD) {
                for (uint8_t ch = 0; ch < kMdCount; ch++) {
                    if (md_[ch].fb_seen || md_[ch].mode != detail::MdMode::None) onBoardRestart(ch);
                }
            }
        } else if (f.id >= 0x320 && f.id <= 0x33F) {
            handleMdFeedback(f, now);
        } else if (f.id >= 0x340 && f.id < 0x340 + kControllerCount) {
            handleController(f, now);
        } else if (f.id == mas_can_setting_id(MAS_CAN_KIND_MD, 1)) {
            handleSettingResponse(f);
        }
    }
}

void Bus::handleMdFeedback(const Frame &f, uint32_t now) {
    uint8_t ch;
    mas_can_md_fb_t raw;
    if (!mas_can_md_fb_decode(&f, &ch, &raw)) return;
    detail::MdState &m = md_[ch];
    Md::Feedback &fb = m.fb;

    if (!m.fb_seen) {
        fb.position = mas_can_u24_diff(raw.position, 0);  // 符号付きの 24 bit として始める
    } else if (!m.relink) {
        fb.position += mas_can_u24_diff(raw.position, m.last_raw_pos);
    }
    m.relink = false;
    m.last_raw_pos = raw.position;
    m.fb_seen = true;

    bool current_ok = !(raw.status & MAS_CAN_MD_ST_CURRENT_INVALID);
    bool speed_ok = !(raw.status & MAS_CAN_MD_ST_SPEED_INVALID);
    fb.status = raw.status;
    fb.current = current_ok ? raw.current * m.current_lsb_ma * 0.001f : 0.0f;
    fb.speed = speed_ok ? raw.speed * m.speed_lsb_rpm * (kTwoPi / 60.0f) : 0.0f;
    fb.duty = (current_ok && speed_ok)
                  ? 0.0f
                  : mas_can_md_duty_value(raw.duty) / (float)MAS_CAN_MD_DUTY_FULL;
    fb.receivedUs = now;
    fb.online = true;

    if (raw.status & MAS_CAN_MD_ST_FAULT) {
        if (!m.fault_known && !m.fault_reading) {
            detail::SettingReq r{MAS_CAN_KIND_MD,           ch,  MAS_CAN_OP_READ,
                                 MAS_CAN_ITEM_FAULT_DETAIL, {0}, detail::ReqTag::ReadFault};
            if (queueSetting(r)) m.fault_reading = true;
        }
    } else {
        m.fault_known = false;
        m.fault_detail = 0;
    }
}

void Bus::handleController(const Frame &f, uint32_t now) {
    if (f.rtr || f.dlc != 8) return;
    detail::ControllerState &p = pad_[f.id - 0x340];
    for (int i = 0; i < 4; i++) p.axis[i] = (int8_t)f.data[i];
    p.trigger[0] = f.data[4];
    p.trigger[1] = f.data[5];
    uint16_t bits = mas_can_get_u16(&f.data[6]);
    uint16_t buttons = bits & 0x0FFFu;
    p.pressed |= buttons & ~p.buttons;
    p.released |= p.buttons & ~buttons;
    p.buttons = buttons;
    p.link = (bits & (1u << 12)) != 0;
    p.last_us = now;
    p.seen = true;
}

void Bus::handleSettingResponse(const Frame &f) {
    if (!req_active_) return;
    const detail::SettingReq &req = queue_[q_head_];
    mas_can_setting_t s;
    if (!mas_can_setting_decode(&f, &s)) return;
    if (s.ch != req.ch || s.item != req.item || s.op != req.op || s.seq != req_seq_) return;

    req_active_ = false;
    q_head_ = (uint8_t)((q_head_ + 1) % kQueue);
    q_len_--;

    detail::MdState &m = md_[req.ch];
    Md md(this, req.ch);
    switch (req.tag) {
        case detail::ReqTag::CurrentLsb:
        case detail::ReqTag::SpeedLsb: {
            bool cur = req.tag == detail::ReqTag::CurrentLsb;
            if (s.result == MAS_CAN_RES_OK) {
                (cur ? m.current_lsb_pending : m.speed_lsb_pending) = false;
            } else if (setting_error_handler_) {
                setting_error_handler_(md, req.item, s.result);
            }
            break;
        }
        case detail::ReqTag::ReadFault: {
            m.fault_reading = false;
            if (s.result != MAS_CAN_RES_OK) break;
            uint32_t detail = mas_can_value_u32(s.value);
            m.fault_known = true;
            if (detail & MAS_CAN_FAULT_RESTARTED) {
                onBoardRestart(req.ch);
                detail::SettingReq c{MAS_CAN_KIND_MD,           req.ch, MAS_CAN_OP_WRITE,
                                     MAS_CAN_ITEM_FAULT_DETAIL, {0},    detail::ReqTag::ClearFault};
                mas_can_value_set_u32(c.value, MAS_CAN_FAULT_RESTARTED);
                queueSetting(c);
            }
            uint32_t others = detail & ~MAS_CAN_FAULT_RESTARTED;
            uint32_t fresh = others & ~m.fault_detail;
            m.fault_detail = others;
            if (fresh && fault_handler_) fault_handler_(md, fresh);
            break;
        }
        default:
            break;
    }
}

void Bus::onBoardRestart(uint8_t ch) {
    md_[ch].relink = true;  // 基板の位置は 0 から始まり直すので、積み上げた位置を続ける
    queueLsbWrites(ch);
}

bool Bus::chReady(uint8_t ch) const {
    return !md_[ch].current_lsb_pending && !md_[ch].speed_lsb_pending;
}

int16_t Bus::rawCommand(uint8_t ch) const {
    const detail::MdState &m = md_[ch];
    int16_t stop = mas_can_md_stop_value(toCmd(m.mode));
    if (estopped() || !chReady(ch)) return stop;
    if (m.raw) return m.raw_value;
    switch (m.mode) {
        case detail::MdMode::Current:
            return mas_can_sat16(m.value * 1000.0f / m.current_lsb_ma);
        case detail::MdMode::Speed:
            return mas_can_sat16(m.value * (60.0f / kTwoPi) / m.speed_lsb_rpm);
        case detail::MdMode::Duty:
            return mas_can_md_duty_pack(mas_can_sat16(m.value * MAS_CAN_MD_DUTY_FULL), m.brake);
        default:
            return stop;
    }
}

void Bus::sendCommands() {
    for (uint8_t base = 0; base < kMdCount; base += MAS_CAN_MD_SLOTS) {
        detail::MdMode primary = detail::MdMode::None;
        for (uint8_t i = 0; i < MAS_CAN_MD_SLOTS; i++) {
            detail::MdState &m = md_[base + i];
            if (m.mode == detail::MdMode::None) continue;
            if (primary == detail::MdMode::None) primary = m.mode;
            m.conflict = m.mode != primary;
        }
        if (primary == detail::MdMode::None) continue;

        Frame f;
        bool used[MAS_CAN_MD_SLOTS] = {false, false, false, false};
        mas_can_md_cmd_init(&f, toCmd(primary), base);
        for (uint8_t i = 0; i < MAS_CAN_MD_SLOTS; i++) {
            const detail::MdState &m = md_[base + i];
            if (m.mode != primary) continue;
            mas_can_md_cmd_set(&f, base + i, rawCommand(base + i));
            used[i] = true;
        }
        mas_can_md_cmd_trim(&f, used);
        port_->send(f);
    }
}

void Bus::serviceEstop(uint32_t now) {
    if (estop_own_ && elapsed(now, last_estop_us_, kEstopPeriodUs)) {
        Frame f{};
        f.id = MAS_CAN_ID_ESTOP;
        if (port_->send(f)) last_estop_us_ = now;
    }
    if (clear_checking_ && (int32_t)(now - clear_check_us_) >= 0) {
        bool latched = false;
        for (const auto &m : md_) latched |= m.fb.online && m.fb.estopLatched();
        if (latched && clear_retries_ < kClearRetries) {
            Frame f;
            mas_can_estop_clear_encode(&f);
            port_->send(f);
            clear_retries_++;
            clear_check_us_ = now + kEstopPeriodUs;
        } else {
            clear_checking_ = false;
        }
    }
}

void Bus::estop() {
    estop_own_ = true;
    last_estop_us_ = port_ ? port_->micros() - kEstopPeriodUs : 0;
}

void Bus::clearEstop() {
    estop_own_ = false;
    estop_seen_ = false;
    for (auto &m : md_) {  // 指令を止める値に戻してから解除する（D21）
        m.raw = false;
        m.value = 0;
    }
    if (!port_) return;
    Frame f;
    mas_can_estop_clear_encode(&f);
    port_->send(f);
    clear_retries_ = 0;
    clear_checking_ = true;
    clear_check_us_ = port_->micros() + kEstopPeriodUs;
}

bool Bus::estopped() const { return estop_own_ || estop_seen_; }

bool Bus::queueSetting(const detail::SettingReq &req) {
    // 同じ ch・同じ目的の要求が待っていれば、値だけ差し替える（送信中のものは除く）
    for (uint8_t i = req_active_ ? 1 : 0; i < q_len_; i++) {
        detail::SettingReq &q = queue_[(q_head_ + i) % kQueue];
        if (q.tag == req.tag && q.ch == req.ch && q.tag != detail::ReqTag::User) {
            q = req;
            return true;
        }
    }
    if (q_len_ >= kQueue) return false;
    queue_[(q_head_ + q_len_) % kQueue] = req;
    q_len_++;
    return true;
}

void Bus::queueLsbWrites(uint8_t ch) {
    detail::MdState &m = md_[ch];
    if (m.current_lsb_set) {
        detail::SettingReq r{MAS_CAN_KIND_MD, ch,  MAS_CAN_OP_WRITE,
                             kItemCurrentLsb, {0}, detail::ReqTag::CurrentLsb};
        mas_can_value_set_f32(r.value, m.current_lsb_ma);
        if (queueSetting(r)) m.current_lsb_pending = true;
    }
    if (m.speed_lsb_set) {
        detail::SettingReq r{MAS_CAN_KIND_MD, ch,  MAS_CAN_OP_WRITE,
                             kItemSpeedLsb,   {0}, detail::ReqTag::SpeedLsb};
        mas_can_value_set_f32(r.value, m.speed_lsb_rpm);
        if (queueSetting(r)) m.speed_lsb_pending = true;
    }
}

void Bus::serviceSettings(uint32_t now) {
    if (req_active_) {
        const detail::SettingReq &req = queue_[q_head_];
        uint32_t timeout = (req.op == MAS_CAN_OP_SAVE || req.op == MAS_CAN_OP_INIT)
                               ? kReqSaveTimeoutUs
                               : kReqTimeoutUs;
        if (!elapsed(now, req_deadline_us_ - timeout, timeout)) return;
        if (req_retries_ < kReqRetries) {
            req_retries_++;
            req_active_ = false;  // 下で送り直す
        } else {
            req_active_ = false;
            detail::SettingReq failed = req;
            q_head_ = (uint8_t)((q_head_ + 1) % kQueue);
            q_len_--;
            if (failed.tag == detail::ReqTag::ReadFault) md_[failed.ch].fault_reading = false;
            if ((failed.tag == detail::ReqTag::CurrentLsb ||
                 failed.tag == detail::ReqTag::SpeedLsb) &&
                setting_error_handler_) {
                Md md(this, failed.ch);
                setting_error_handler_(md, failed.item, 255);
            }
            req_retries_ = 0;
        }
    }
    if (req_active_ || q_len_ == 0) return;

    const detail::SettingReq &req = queue_[q_head_];
    mas_can_setting_t s{};
    s.kind = (mas_can_kind_t)req.kind;
    s.response = false;
    s.ch = req.ch;
    s.op = (mas_can_op_t)req.op;
    s.item = req.item;
    for (int i = 0; i < 4; i++) s.value[i] = req.value[i];
    s.seq = ++req_seq_;
    Frame f;
    mas_can_setting_encode(&s, &f);
    if (!port_->send(f)) return;
    uint32_t timeout = (req.op == MAS_CAN_OP_SAVE || req.op == MAS_CAN_OP_INIT) ? kReqSaveTimeoutUs
                                                                                : kReqTimeoutUs;
    req_deadline_us_ = now + timeout;
    req_active_ = true;
}

bool Bus::waitOnline(const Md *mds, size_t count, uint32_t timeout_ms) {
    if (!started_) return false;
    uint32_t start = port_->micros();
    while (!elapsed(port_->micros(), start, timeout_ms * 1000u)) {
        update();
        bool all = true;
        for (size_t i = 0; i < count; i++) all &= mds[i].online();
        if (all) return true;
    }
    return false;
}

void Bus::onFault(FaultHandler handler) { fault_handler_ = handler; }
void Bus::onSettingError(SettingErrorHandler handler) { setting_error_handler_ = handler; }
void Bus::setSyncSource(bool enable) { sync_source_ = enable; }

}  // namespace can
}  // namespace mas
