#include "mas/can/host.h"

#include <new>

#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"
#include "state.h"

namespace mas {
namespace can {

using detail::ControllerState;
using detail::DeviceState;
using detail::MdMode;
using detail::MdState;
using detail::ReqTag;
using detail::ServoState;
using detail::SettingReq;

namespace {

constexpr uint32_t kMdPeriodUs = 10000;     // MD の指令 100 Hz（C-44）
constexpr uint32_t kServoPeriodUs = 20000;  // サーボ 50 Hz（S-6）
constexpr uint32_t kSyncPeriodUs = 50000;   // SYNC 20 Hz（C-33）
constexpr uint32_t kEstopPeriodUs = 100000; // E-stop 10 Hz（C-31）
constexpr uint32_t kOnlineUs = 200000;      // FB がこれだけ来なければ online でない
constexpr uint32_t kReqTimeoutUs = 50000;
constexpr uint8_t kReqRetries = 2;
constexpr uint8_t kClearRetries = 3;
constexpr uint32_t kLoadWindowUs = 1000000;
constexpr float kTwoPi = 6.28318530718f;
constexpr uint32_t kLsbRetryUs = 1000000; // 刻みの書き込みに失敗したら、これだけ待って書き直す
constexpr uint8_t kItemCurrentLsb = 48;
constexpr uint8_t kItemSpeedLsb = 49;

bool elapsed(uint32_t now, uint32_t since, uint32_t period) {
    return (uint32_t)(now - since) >= period;
}

unsigned maxCh(Kind kind) {
    switch (kind) {
        case Kind::Md:
            return MAS_CAN_MD_MAX_CH;
        case Kind::Controller:
            return MAS_CAN_CONTROLLER_MAX_CH;
        case Kind::Servo:
            return MAS_CAN_SERVO_MAX_CH;
        default:
            return 256; // 汎用ピンなどは線の上の形が未決
    }
}

mas_can_md_cmd_t toCmd(MdMode m) {
    switch (m) {
        case MdMode::Current:
            return MAS_CAN_MD_CMD_CURRENT;
        case MdMode::Speed:
            return MAS_CAN_MD_CMD_SPEED;
        default:
            return MAS_CAN_MD_CMD_DUTY;
    }
}

bool ready(const MdState& m) { return !m.current_pending && !m.speed_pending; }

} // namespace

const char* toString(Error e) {
    switch (e) {
        case Error::None:
            return "";
        case Error::PortStartFailed:
            return "port start failed";
        case Error::OutOfMemory:
            return "out of memory";
        case Error::BusLoadHigh:
            return "bus load high";
        case Error::CommandConflict:
            return "command conflict";
    }
    return "unknown error";
}

uint8_t Device::ch() const { return s_ ? s_->ch : 255; }
Kind Device::kind() const { return s_ ? s_->kind : Kind::Md; }

// ---- 取っ手 ----

DeviceState* Host::find(Kind kind, uint8_t ch) const {
    for (DeviceState* s = head_; s; s = s->next) {
        if (s->kind == kind && s->ch == ch) return s;
    }
    return nullptr;
}

DeviceState* Host::obtain(Kind kind, uint8_t ch) {
    if (ch >= maxCh(kind)) return nullptr;
    if (DeviceState* s = find(kind, ch)) return s;
    DeviceState* s = nullptr;
    switch (kind) {
        case Kind::Md:
            s = new (std::nothrow) MdState();
            break;
        case Kind::Controller:
            s = new (std::nothrow) ControllerState();
            break;
        case Kind::Servo:
            s = new (std::nothrow) ServoState();
            break;
        case Kind::Pin:
            s = new (std::nothrow) detail::PinState();
            break;
        case Kind::Air:
            s = new (std::nothrow) detail::AirState();
            break;
        case Kind::Solenoid:
            s = new (std::nothrow) detail::SolenoidState();
            break;
    }
    if (!s) {
        error_ = Error::OutOfMemory;
        return nullptr;
    }
    s->host = this;
    s->kind = kind;
    s->ch = ch;
    s->next = head_;
    head_ = s;
    return s;
}

Md Host::md(uint8_t ch) { return Md(obtain(Kind::Md, ch)); }
Controller Host::controller(uint8_t ch) { return Controller(obtain(Kind::Controller, ch)); }
Servo Host::servo(uint8_t ch) { return Servo(obtain(Kind::Servo, ch)); }
Pin Host::pin(uint8_t ch) { return Pin(obtain(Kind::Pin, ch)); }
Air Host::air(uint8_t ch) { return Air(obtain(Kind::Air, ch)); }
Solenoid Host::solenoid(uint8_t ch) { return Solenoid(obtain(Kind::Solenoid, ch)); }

// ---- 初期化と送受信 ----

bool Host::begin(Port& port) {
    port_ = &port;
    uint32_t now = port_->micros();
    for (auto& t : md_sent_us_) t = now - kMdPeriodUs;
    for (auto& t : servo_sent_us_) t = now - kServoPeriodUs;
    last_sync_us_ = now - kSyncPeriodUs;
    load_start_us_ = now;
    started_ = true;
    return error_ == Error::None; // begin より前の確保の失敗も知らせる（API-6）
}

bool Host::send(const Frame& f) {
    if (!port_->send(f)) return false;
    countBits(f);
    return true;
}

// 1 本の長さの目安: 47 bit ＋ 8 × DLC、スタッフィングは詰める部分の 1 割と見る（API-37）
void Host::countBits(const Frame& f) {
    uint32_t stuffable = 34u + 8u * f.dlc;
    load_bits_ += 47u + 8u * f.dlc + stuffable / 10u;
}

void Host::updateBusLoad(uint32_t now) {
    uint32_t span = now - load_start_us_;
    if (span < kLoadWindowUs) return;
    bus_load_ = (float)load_bits_ / (float)span; // 1 Mbps なので bit/µs がそのまま使用率
    load_bits_ = 0;
    load_start_us_ = now;
    if (load_warn_ > 0 && bus_load_ > load_warn_) error_ = Error::BusLoadHigh;
}

void Host::update() {
    if (!started_) return;
    uint32_t now = port_->micros();

    for (DeviceState* s = head_; s; s = s->next) {
        if (s->kind == Kind::Controller) {
            auto* p = static_cast<ControllerState*>(s);
            p->pressed = p->released = 0;
        }
    }
    receiveAll(now);

    for (DeviceState* s = head_; s; s = s->next) {
        if (s->kind == Kind::Md) {
            auto* m = static_cast<MdState*>(s);
            if (m->fb.online && elapsed(now, m->fb.receivedUs, kOnlineUs)) m->fb.online = false;
        } else if (s->kind == Kind::Controller) {
            auto* p = static_cast<ControllerState*>(s);
            if (p->online && elapsed(now, p->last_us, kOnlineUs)) { // 受信機が落ちた（API-28）
                p->online = p->link = false;
                p->released |= p->buttons;
                p->buttons = 0;
                for (auto& a : p->axis) a = 0;
                p->trigger[0] = p->trigger[1] = 0;
            }
        }
    }

    serviceEstop(now);
    sendMd(false, now);
    sendServos(false, now);
    if (elapsed(now, last_sync_us_, kSyncPeriodUs)) {
        Frame f;
        mas_can_sync_encode(port_->micros(), &f); // 送る直前に取る（C-35）
        if (send(f)) last_sync_us_ = now;
    }
    serviceSettings(now);
    updateBusLoad(now);
}

void Host::flush() {
    if (!started_) return;
    uint32_t now = port_->micros();
    sendMd(true, now);
    sendServos(true, now);
}

void Host::receiveAll(uint32_t now) {
    Frame f;
    while (port_->receive(f)) {
        countBits(f);
        mas_can_kind_t kind;
        if (mas_can_is_estop(&f)) {
            estop_seen_ = true;
        } else if (mas_can_is_estop_clear(&f)) {
            if (estop_seen_ && !estop_own_) resetCommands(); // API-48
            estop_seen_ = false; // 自分の E-stop は自分が解除するまで続く（API-23）
        } else if (mas_can_boot_decode(&f, &kind)) {
            if (kind == MAS_CAN_KIND_MD) { // どの基板かは分からないので、FB を見た MD 全部（C-88）
                for (DeviceState* s = head_; s; s = s->next) {
                    MdState* m = detail::asMd(s);
                    if (m && m->fb_seen) onMdRestart(m);
                }
            }
        } else if (f.id >= 0x320 && f.id < 0x320 + MAS_CAN_MD_MAX_CH) {
            handleMdFeedback(f, now);
        } else if (f.id >= 0x340 && f.id < 0x340 + MAS_CAN_CONTROLLER_MAX_CH) {
            handleController(f, now);
        } else if (f.id == mas_can_setting_id(MAS_CAN_KIND_MD, 1)) {
            handleSettingResponse(f);
        }
    }
}

// ---- MD ----

void Host::handleMdFeedback(const Frame& f, uint32_t now) {
    uint8_t ch;
    mas_can_md_fb_t raw;
    if (!mas_can_md_fb_decode(&f, &ch, &raw)) return;
    MdState* m = detail::asMd(find(Kind::Md, ch));
    if (!m) return; // 求めていない ch の FB は捨てる
    Md::Feedback& fb = m->fb;

    if (!m->fb_seen) {
        fb.position = mas_can_u24_diff(raw.position, 0);
        m->fb_seen = true; // ここから刻みを書きに行く（API-44。pickResolution）
    } else if (!m->relink) {
        fb.position += mas_can_u24_diff(raw.position, m->last_raw_pos);
    }
    m->relink = false;
    m->last_raw_pos = raw.position;

    bool current_ok = !(raw.status & MAS_CAN_MD_ST_CURRENT_INVALID);
    bool speed_ok = !(raw.status & MAS_CAN_MD_ST_SPEED_INVALID);
    fb.status = raw.status;
    fb.current = current_ok ? raw.current * m->current_lsb_ma * 0.001f : 0.0f;
    fb.speed = speed_ok ? raw.speed * m->speed_lsb_rpm * (kTwoPi / 60.0f) : 0.0f;
    fb.duty = (current_ok && speed_ok)
                  ? 0.0f
                  : mas_can_md_duty_value(raw.duty) / (float)MAS_CAN_MD_DUTY_FULL;
    fb.receivedUs = now;
    fb.online = true;

    if (raw.status & MAS_CAN_MD_ST_FAULT) {
        if (!m->fault_known && !m->fault_reading) {
            SettingReq r;
            r.dev = m;
            r.op = MAS_CAN_OP_READ;
            r.item = MAS_CAN_ITEM_FAULT_DETAIL;
            r.tag = ReqTag::ReadFault;
            if (queueSetting(r)) m->fault_reading = true;
        }
    } else {
        m->fault_known = false;
        m->fault_detail = 0;
    }
}

void Host::onMdRestart(DeviceState* s) {
    MdState* m = detail::asMd(s);
    if (!m) return;
    m->relink = true; // 基板の位置は 0 から始まり直すので、積み上げた位置を続ける
    m->current_pending = m->speed_pending = true; // ほかの設定より先に送り直す（C-88）
    m->lsb_hold = false;
}

// 書く必要のある刻みを 1 つ選ぶ。印（pending）が正本なので、列が溢れても取りこぼさない。
// 失敗した ch は少し待ってから書き直す（基板が見えている間）
bool Host::pickResolution(uint32_t now, SettingReq& out) {
    for (DeviceState* s = head_; s; s = s->next) {
        MdState* m = detail::asMd(s);
        if (!m || !m->fb_seen || !m->fb.online || (!m->speed_pending && !m->current_pending))
            continue;
        if (m->lsb_hold && !elapsed(now, m->lsb_retry_us, kLsbRetryUs)) continue;
        m->lsb_hold = false;
        bool speed = m->speed_pending;
        out = SettingReq();
        out.dev = m;
        out.op = MAS_CAN_OP_WRITE;
        out.item = speed ? kItemSpeedLsb : kItemCurrentLsb;
        out.tag = speed ? ReqTag::SpeedLsb : ReqTag::CurrentLsb;
        mas_can_value_set_f32(out.value, speed ? m->speed_lsb_rpm : m->current_lsb_ma);
        return true;
    }
    return false;
}

void Host::sendMd(bool only_dirty, uint32_t now) {
    for (uint8_t b = 0; b < MAS_CAN_MD_MAX_CH / MAS_CAN_MD_SLOTS; b++) {
        MdState* slot[MAS_CAN_MD_SLOTS] = {};
        MdMode primary = MdMode::None;
        bool dirty = false;
        for (uint8_t i = 0; i < MAS_CAN_MD_SLOTS; i++) {
            MdState* m = detail::asMd(find(Kind::Md, (uint8_t)(b * MAS_CAN_MD_SLOTS + i)));
            if (!m || m->mode == MdMode::None) continue;
            slot[i] = m;
            if (primary == MdMode::None) primary = m->mode; // いちばん小さい ch の種類（API-52）
            m->conflict = m->mode != primary;
            if (m->conflict) error_ = Error::CommandConflict;
            dirty |= m->dirty;
        }
        if (primary == MdMode::None) continue;
        if (only_dirty ? !dirty : !elapsed(now, md_sent_us_[b], kMdPeriodUs)) continue;

        Frame f;
        bool used[MAS_CAN_MD_SLOTS] = {false, false, false, false};
        uint8_t base = (uint8_t)(b * MAS_CAN_MD_SLOTS);
        mas_can_md_cmd_init(&f, toCmd(primary), base);
        for (uint8_t i = 0; i < MAS_CAN_MD_SLOTS; i++) {
            MdState* m = slot[i];
            if (!m) continue;
            if (m->conflict) {
                m->dirty = false; // 送れない ch で flush を繰り返さない
                continue;
            }
            int16_t v = 0; // 止める値（電流、速度、duty はどれも 0）
            if (!estopped() && ready(*m)) {
                if (m->raw) {
                    v = m->raw_value;
                } else if (m->mode == MdMode::Current) {
                    v = mas_can_sat16(m->value * 1000.0f / m->current_lsb_ma);
                } else if (m->mode == MdMode::Speed) {
                    v = mas_can_sat16(m->value * (60.0f / kTwoPi) / m->speed_lsb_rpm);
                } else {
                    v = mas_can_md_duty_pack(mas_can_sat16(m->value * MAS_CAN_MD_DUTY_FULL),
                                             m->brake);
                }
            }
            mas_can_md_cmd_set(&f, (uint8_t)(base + i), v);
            used[i] = true;
            m->dirty = false;
        }
        mas_can_md_cmd_trim(&f, used);
        if (send(f)) md_sent_us_[b] = now;
    }
}

// ---- サーボ ----

void Host::sendServos(bool only_dirty, uint32_t now) {
    for (uint8_t b = 0; b < MAS_CAN_SERVO_MAX_CH / MAS_CAN_SERVO_SLOTS; b++) {
        ServoState* slot[MAS_CAN_SERVO_SLOTS] = {};
        bool any = false, dirty = false;
        for (uint8_t i = 0; i < MAS_CAN_SERVO_SLOTS; i++) {
            DeviceState* s = find(Kind::Servo, (uint8_t)(b * MAS_CAN_SERVO_SLOTS + i));
            if (!s) continue;
            slot[i] = static_cast<ServoState*>(s);
            any = true;
            dirty |= slot[i]->dirty;
        }
        if (!any) continue;
        if (only_dirty ? !dirty : !elapsed(now, servo_sent_us_[b], kServoPeriodUs)) continue;

        Frame f;
        f.id = mas_can_servo_cmd_id((uint8_t)(b * MAS_CAN_SERVO_SLOTS));
        f.rtr = false;
        f.dlc = 0;
        for (uint8_t i = 0; i < MAS_CAN_SERVO_SLOTS; i++) {
            f.data[i] = MAS_CAN_SERVO_STOP; // 使っていない slot は止める値（C-40、S-3）
            if (!slot[i]) continue;
            if (!estopped()) f.data[i] = slot[i]->value;
            slot[i]->dirty = false;
            f.dlc = (uint8_t)(i + 1); // 後ろの使っていない slot は削る（C-41）
        }
        if (send(f)) servo_sent_us_[b] = now;
    }
}

// ---- コントローラ ----

void Host::handleController(const Frame& f, uint32_t now) {
    if (f.rtr || f.dlc != 8) return;
    DeviceState* s = find(Kind::Controller, (uint8_t)(f.id - 0x340));
    if (!s) return;
    auto* p = static_cast<ControllerState*>(s);
    for (int i = 0; i < 4; i++) p->axis[i] = (int8_t)f.data[i];
    p->trigger[0] = f.data[4];
    p->trigger[1] = f.data[5];
    uint16_t bits = mas_can_get_u16(&f.data[6]);
    uint16_t buttons = bits & 0xCFFFu; // bit 12 つながっている、bit 13 異常あり
    p->pressed |= buttons & ~p->buttons;
    p->released |= p->buttons & ~buttons;
    p->buttons = buttons;
    p->link = (bits & (1u << 12)) != 0;
    p->last_us = now;
    p->online = p->seen = true;
}

// ---- 設定の要求（1 つずつ。C-51） ----

bool Host::queueSetting(const SettingReq& req) {
    for (uint8_t i = 0; i < q_len_; i++) {
        SettingReq& q = queue_[i];
        if (q.tag == req.tag && q.dev == req.dev) { // 同じ目的が待っていれば値だけ差し替える
            q = req;
            return true;
        }
    }
    if (q_len_ >= kQueue) return false;
    queue_[q_len_++] = req;
    return true;
}

void Host::finishActive() {
    if (active_from_queue_) {
        for (uint8_t i = 1; i < q_len_; i++) queue_[i - 1] = queue_[i];
        if (q_len_) q_len_--;
    }
    req_active_ = false;
    req_retries_ = 0;
}

void Host::handleSettingResponse(const Frame& f) {
    if (!req_active_) return;
    const SettingReq req = active_;
    mas_can_setting_t s;
    if (!mas_can_setting_decode(&f, &s)) return;
    if (s.ch != req.dev->ch || s.item != req.item || s.op != req.op || s.seq != req_seq_) return;
    finishActive();

    MdState* m = detail::asMd(req.dev);
    if (!m) return;
    Device dev(m);
    switch (req.tag) {
        case ReqTag::CurrentLsb:
        case ReqTag::SpeedLsb: {
            bool cur = req.tag == ReqTag::CurrentLsb;
            float& lsb = cur ? m->current_lsb_ma : m->speed_lsb_rpm;
            bool& pending = cur ? m->current_pending : m->speed_pending;
            if (s.result != MAS_CAN_RES_OK) { // 断られた。直るまで止める値のまま
                m->lsb_hold = true;
                m->lsb_retry_us = port_->micros();
                if (setting_error_handler_) setting_error_handler_(dev, s.result);
                break;
            }
            float sent = mas_can_value_f32(req.value);
            if (lsb != sent) break; // 待っている間にアプリが値を変えた。次に書き直す
            float got = mas_can_value_f32(s.value);
            pending = false;
            if (got != sent && got > 0) { // 基板が抑えた（M-43）。基板の値で換算し、知らせる
                lsb = got;
                if (setting_error_handler_) setting_error_handler_(dev, MAS_CAN_RES_OK);
            }
            break;
        }
        case ReqTag::ReadFault: {
            m->fault_reading = false;
            if (s.result != MAS_CAN_RES_OK) break;
            uint32_t bits = mas_can_value_u32(s.value);
            m->fault_known = true;
            if (bits & MAS_CAN_FAULT_RESTARTED) { // 再起動の bit はライブラリが消す（API-19）
                onMdRestart(m);
                SettingReq c;
                c.dev = m;
                c.op = MAS_CAN_OP_WRITE;
                c.item = MAS_CAN_ITEM_FAULT_DETAIL;
                c.tag = ReqTag::ClearFault;
                mas_can_value_set_u32(c.value, MAS_CAN_FAULT_RESTARTED);
                queueSetting(c);
            }
            uint32_t others = bits & ~(uint32_t)MAS_CAN_FAULT_RESTARTED;
            uint32_t fresh = others & ~m->fault_detail;
            m->fault_detail = others;
            if (fresh && fault_handler_) fault_handler_(dev, fresh);
            break;
        }
        case ReqTag::ClearFault:
            break;
    }
}

void Host::serviceSettings(uint32_t now) {
    if (req_active_) {
        if (!elapsed(now, req_sent_us_, kReqTimeoutUs)) return;
        if (req_retries_ < kReqRetries) {
            req_retries_++; // 下で同じ要求を送り直す
        } else {
            const SettingReq failed = active_;
            finishActive();
            MdState* m = detail::asMd(failed.dev);
            if (m && failed.tag == ReqTag::ReadFault) m->fault_reading = false;
            if (m && (failed.tag == ReqTag::CurrentLsb || failed.tag == ReqTag::SpeedLsb)) {
                m->lsb_hold = true; // 少し待って書き直す
                m->lsb_retry_us = now;
                if (setting_error_handler_) {
                    Device dev(m);
                    setting_error_handler_(dev, 255);
                }
            }
            return;
        }
    } else {
        // プログラムの値を先に（C-88）、なければ列の先頭
        if (pickResolution(now, active_)) {
            active_from_queue_ = false;
        } else if (q_len_) {
            active_ = queue_[0];
            active_from_queue_ = true;
        } else {
            return;
        }
        req_retries_ = 0;
    }

    mas_can_setting_t s{};
    s.kind = MAS_CAN_KIND_MD;
    s.response = false;
    s.ch = active_.dev->ch;
    s.op = (mas_can_op_t)active_.op;
    s.item = active_.item;
    for (int i = 0; i < 4; i++) s.value[i] = active_.value[i];
    s.seq = ++req_seq_;
    Frame f;
    mas_can_setting_encode(&s, &f);
    req_active_ = true;
    req_sent_us_ = now;
    if (!send(f)) req_sent_us_ = now - kReqTimeoutUs; // 送れなければ次の update で送り直す
}

// ---- E-stop ----

void Host::resetCommands() { // アプリの指令を止める値に戻す（API-22）
    for (DeviceState* s = head_; s; s = s->next) {
        if (MdState* m = detail::asMd(s)) {
            m->raw = false;
            m->value = 0;
        } else if (s->kind == Kind::Servo) {
            static_cast<ServoState*>(s)->value = MAS_CAN_SERVO_STOP;
        }
    }
}

void Host::serviceEstop(uint32_t now) {
    if (estop_own_ && elapsed(now, last_estop_us_, kEstopPeriodUs)) {
        Frame f{};
        f.id = MAS_CAN_ID_ESTOP;
        if (send(f)) last_estop_us_ = now;
    }
    if (clear_checking_ && elapsed(now, clear_check_us_, kEstopPeriodUs)) {
        bool latched = false;
        for (DeviceState* s = head_; s; s = s->next) {
            MdState* m = detail::asMd(s);
            if (m && m->fb.online && m->fb.estopLatched()) latched = true;
        }
        if (latched && clear_retries_ < kClearRetries) { // API-24
            Frame f;
            mas_can_estop_clear_encode(&f);
            send(f);
            clear_retries_++;
            clear_check_us_ = now;
        } else {
            clear_checking_ = false;
        }
    }
}

void Host::estop() {
    estop_own_ = true;
    if (!started_) return;
    Frame f{};
    f.id = MAS_CAN_ID_ESTOP;
    send(f); // すぐ 1 本送り、以後は 10 Hz
    last_estop_us_ = port_->micros();
}

void Host::clearEstop() {
    estop_own_ = false;
    estop_seen_ = false;
    resetCommands();
    if (!started_) return;
    Frame f;
    mas_can_estop_clear_encode(&f);
    send(f);
    clear_retries_ = 0;
    clear_checking_ = true;
    clear_check_us_ = port_->micros();
}

bool Host::waitReady(uint32_t timeout_ms) {
    if (!started_) return false;
    uint32_t start = port_->micros();
    for (;;) {
        update();
        bool all = true;
        for (DeviceState* s = head_; s; s = s->next) {
            if (MdState* m = detail::asMd(s)) {
                all &= m->fb.online && ready(*m);
            } else if (s->kind == Kind::Controller) {
                all &= static_cast<ControllerState*>(s)->online;
            } // FB の無い種類は数えない
        }
        if (all) return true;
        if (elapsed(port_->micros(), start, timeout_ms * 1000u)) return false;
    }
}

} // namespace can
} // namespace mas
