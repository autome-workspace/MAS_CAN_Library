#include "relay.h"

#include <math.h>
#include <string.h>

#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"

namespace relay {

namespace {

constexpr uint32_t kHeartbeatTimeoutUs = 300000;  // Heartbeat（100 ms ごと）が 3 回来ない
constexpr uint32_t kRetryUs = 200000;             // 状態の要求を送り直す間隔
constexpr uint32_t kEnableTimeoutUs = 3000000;
constexpr uint32_t kEstopBrakeUs = 1000000;  // E-stop で速度 0 にしてから Idle にするまでの上限
constexpr float kStoppedVel = 0.2f;          // [turn/s] これより遅ければ止まったとみなす

bool elapsed(uint32_t now, uint32_t since, uint32_t period) {
    return (uint32_t)(now - since) >= period;
}
bool after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }  // a が b より後
float clampf(float v, float lim) { return v > lim ? lim : (v < -lim ? -lim : v); }

// 速度の FF [Nm]
// - 慣性: 前に送ってからの平均の加速度 × 慣性。20 ms ごとに送るので、その間の平均にして目標の段差ごとに
//   FF が跳ねないようにする。加速度の制限が無いと目標が飛ぶので載せない
// - 摩擦: 目標の速度の向きに一定のトルク。0 の近く（kFrictionBand）では比例で小さくして、0 で跳ねない。
//   測った速度ではなく目標で決める（測った値で決めると止まりかけで向きが振れる）
constexpr float kFrictionBand = 0.1f;  // [turn/s] モータ軸
float torqueFf(const Channel& c, uint32_t now, const Config& cfg) {
    float ff = 0;
    uint32_t dt = now - c.cmd_at;
    if (c.ramping && cfg.inertia > 0 && dt > 0)
        ff += cfg.inertia * (c.target - c.sent_target) / ((float)dt * 1e-6f);
    if (cfg.friction > 0) ff += cfg.friction * clampf(c.target / kFrictionBand, 1.0f);
    return clampf(ff, (float)cfg.current_limit_ma / 1000.0f * cfg.kt);
}

// XDrive のエラー → MAS の異常の詳細（M-44: 8 過電流 9 過熱 10 低電圧 11 過電圧 12 エンコーダ
// 13 ゲートドライバ）
uint32_t faultBits(const Channel& c) {
    uint32_t b = 0;
    if (c.axis_error & xd::kErrUnderVoltage) b |= 1u << 10;
    if (c.axis_error & xd::kErrOverVoltage) b |= 1u << 11;
    if (c.axis_error & xd::kErrOverTemp) b |= 1u << 9;
    if (c.axis_error & xd::kErrEncoderFailed) b |= 1u << 12;
    if (c.motor_error & xd::kMotorDrvFault) b |= 1u << 13;
    if (c.motor_error & (xd::kMotorDcBusOverCurrent | xd::kMotorCurrentLimitViolation))
        b |= 1u << 8;
    return b;
}

// 上の表に当てはまらない Fault（OVERSPEED、Closed loop から外れた、Heartbeat の途絶 …）。
// 異常の詳細が 0 のままだと、主制御には FAULT の bit が見えず、ライブラリの clearFault も何も送らない
// （＝ E-stop の解除でしか解けない）。M-44 では bit 14〜31 は予約。仮に 31 を使う（README「チームで決めること」）
constexpr uint32_t kFaultOther = 1u << 31;
constexpr uint32_t kDetailsWaitUs = 500000;  // motor / encoder のエラーを待つ上限（届かなくても kFaultOther を立てる）

// 自分で戻してよい Fault（主制御に解除を求めない）。電源の入れ直し・瞬断・低電圧・制御周期の遅れ。
// OVERSPEED・過電流・エンコーダなど、原因が残りうるものは解除を待つ（offset のずれで暴走した 2026-10-27）
constexpr uint32_t kAxisSoft = xd::kErrUnderVoltage | xd::kErrWatchdog | xd::kErrEstop |
                               xd::kErrMotorFailed | xd::kErrMotorDisarmed;
constexpr uint32_t kAxisBoot = kAxisSoft | xd::kErrEncoderFailed;  // 再起動した XDrive に立っているもの
constexpr uint32_t kMotorSoft = xd::kMotorControlDeadlineMissed;
// 回数の上限は付けない。XDrive が見えていて、XDrive の側に原因が残っていなければ、何度でも戻す
constexpr uint32_t kRecoverHoldUs = 500000;  // XDrive が止まって見え続けてから戻すまで
// XDrive が起きてから回し始めるまで。起動直後に Closed loop を要求すると CONTROLLER_FAILED と
// CONTROL_DEADLINE_MISSED で落ちる（2026-10-28、最初の Heartbeat から 0.1 s で要求して 4 台とも）
constexpr uint32_t kBootSettleUs = 1000000;

}  // namespace

const char* name(Link l) {
    switch (l) {
        case Link::Unseen: return "unseen";
        case Link::Idle: return "idle";
        case Link::Enabling: return "enabling";
        case Link::Running: return "running";
        case Link::Stopping: return "stopping";
        case Link::Fault: return "fault";
    }
    return "?";
}

const char* name(Why w) {
    switch (w) {
        case Why::None: return "-";
        case Why::HeartbeatLost: return "heartbeat-lost";
        case Why::AxisError: return "axis-error";
        case Why::LeftClosedLoop: return "left-closed-loop";
        case Why::EnableTimeout: return "enable-timeout";
    }
    return "?";
}

void Relay::begin(const Config& cfg, uint32_t now) {
    cfg_ = cfg;
    mas_can_node_config_t nc = cfg.node;
    nc.md_count = kCh;
    nc.md_has_current = true;
    nc.md_has_speed = true;
    nc.md_has_coast = true;
    nc.md_current_limit_max_ma = cfg.current_limit_ma;
    nc.md_current_limit_default_ma = cfg.current_limit_ma;
    nc.md_fb_rate_max_hz = 100;
    mas_can_node_init(&node_, &nc, now);

    // 下のバスの周期。4 台で使用率がおよそ 40% 以下になる値（README の表）
    period_us_ = cfg.xd_baud >= 500000 ? 10000 : 20000;
    for (auto& c : ch_) c = Channel();
    last_tick_ = now;
}

void Relay::ready(uint32_t now) { mas_can_node_ready(&node_, now); }

int Relay::localOfNode(uint8_t node) const {
    for (int i = 0; i < kCh; i++)
        if (cfg_.xd_node[i] == node) return i;
    return -1;
}

// ---- 主バス ----

// node に渡す前に見る。XDrive では実現できない設定の書き込みには、ここで結果 4 を返す。
bool Relay::intercept(const mas_can_frame_t& f) {
    mas_can_setting_t s;
    if (f.id != mas_can_setting_id(MAS_CAN_KIND_MD, 0) || !mas_can_setting_decode(&f, &s))
        return false;
    if (s.response || s.kind != MAS_CAN_KIND_MD || s.op != MAS_CAN_OP_WRITE) return false;
    int local = (int)s.ch - node_.cfg.md_first_ch;
    if (local < 0 || local >= kCh) return false;

    // 38: 向きは転流と一体（M-41）。39〜41: 速度ループは XDrive が持つ（ゲインは odrivetool で）
    if (s.item >= 38 && s.item <= 41) {
        const mas_can_md_settings_t* set = mas_can_md_settings(&node_, (uint8_t)local);
        mas_can_setting_t r = s;
        r.response = true;
        r.result = MAS_CAN_RES_NOT_REALIZABLE;
        if (s.item == 38) mas_can_value_set_u32(r.value, set->encoder_reverse);
        if (s.item == 39) mas_can_value_set_f32(r.value, set->speed_kp);
        if (s.item == 40) mas_can_value_set_f32(r.value, set->speed_ki);
        if (s.item == 41) mas_can_value_set_f32(r.value, set->speed_kd);
        mas_can_frame_t out;
        mas_can_setting_encode(&r, &out);
        pushMasExtra(out);
        return true;
    }
    return false;
}

void Relay::onMas(const mas_can_frame_t& f, uint32_t now) {
    if (intercept(f)) return;
    mas_can_node_on_frame(&node_, &f, now);

    // 異常の詳細（設定 5）の書き込み ＝ 主制御が異常を見たという合図。XDrive の Fault を解く
    mas_can_setting_t s;
    if (f.id == mas_can_setting_id(MAS_CAN_KIND_MD, 0) && mas_can_setting_decode(&f, &s) &&
        !s.response && s.kind == MAS_CAN_KIND_MD && s.op == MAS_CAN_OP_WRITE &&
        s.item == MAS_CAN_ITEM_FAULT_DETAIL && s.ch >= node_.cfg.md_first_ch &&
        s.ch < node_.cfg.md_first_ch + kCh &&
        (mas_can_value_u32(s.value) & ~(uint32_t)MAS_CAN_FAULT_RESTARTED))
        releaseFaults();  // 再起動の bit だけを消す書き込み（ライブラリが自動で送る）では解かない
}

void Relay::pushMasExtra(const mas_can_frame_t& f) {
    if (mas_extra_len_ >= kMasExtra) return;
    mas_extra_[(mas_extra_head_ + mas_extra_len_) % kMasExtra] = f;
    mas_extra_len_++;
}

const mas_can_frame_t* Relay::peekMas() {
    const mas_can_frame_t* f = mas_can_node_peek_tx(&node_);
    from_extra_ = f == nullptr;
    if (!f) return mas_extra_len_ ? &mas_extra_[mas_extra_head_] : nullptr;

    // 下の XDrive が見えない ch の FB は、速度・位置・電流を無効にする（M-18 の bit 1、2）
    uint8_t ch;
    mas_can_md_fb_t fb;
    if (!mas_can_md_fb_decode(f, &ch, &fb)) return f;
    int local = (int)ch - node_.cfg.md_first_ch;
    if (local < 0 || local >= kCh) return f;
    const Channel& c = ch_[local];
    bool hb_ok = c.link != Link::Unseen && !elapsed(last_tick_, c.hb_at, kHeartbeatTimeoutUs);
    bool est_ok = c.est_seen && !elapsed(last_tick_, c.est_rx_at, 5 * period_us_);
    if (hb_ok && est_ok) return f;
    fb.status |= MAS_CAN_MD_ST_SPEED_INVALID | MAS_CAN_MD_ST_CURRENT_INVALID;
    fb.current = fb.speed = 0;
    fb.position = 0;
    mas_can_md_fb_encode(&fb, ch, &fb_patched_);
    return &fb_patched_;
}

void Relay::dropMas() {
    if (!from_extra_) {
        mas_can_node_drop_tx(&node_);
    } else if (mas_extra_len_) {
        mas_extra_head_ = (uint8_t)((mas_extra_head_ + 1) % kMasExtra);
        mas_extra_len_--;
    }
}

// ---- 下のバス ----

void Relay::onXd(const xd::Frame& f, uint32_t now) {
    if (f.rtr) return;
    int i = localOfNode(xd::nodeOf(f.id));
    if (i < 0) return;
    Channel& c = ch_[i];
    switch (xd::cmdOf(f.id)) {
        case xd::kHeartbeat:
            if (f.dlc < 5) return;
            c.axis_error = xd::getU32(f.data);
            c.state = f.data[4];
            if (c.link == Link::Unseen || elapsed(now, c.hb_at, kHeartbeatTimeoutUs)) c.hb_since = now;
            c.hb_at = now;
            c.wait_hb = false;
            if (c.link == Link::Unseen) {
                setLink(i, Link::Idle, now);
                // 中継機が起きて初めて見た XDrive のエラーは、前の中継機の分（Estop の跡など）。1 度だけ消して見直す
                if (c.axis_error) {
                    pushXd(xd::make(cfg_.xd_node[i], xd::kClearErrors, 0));
                    c.wait_hb = true;
                }
            }
            break;
        case xd::kGetEncoderEstimates:
            if (f.dlc < 8) return;
            c.pos = xd::getF32(f.data);
            c.vel = xd::getF32(f.data + 4);
            if (!c.est_seen) c.pos0 = c.pos;
            c.est_seen = true;
            c.est_rx_at = now;
            break;
        case xd::kGetIq:
            if (f.dlc < 8) return;
            c.iq_set = xd::getF32(f.data);
            c.iq = xd::getF32(f.data + 4);
            break;
        case xd::kGetMotorError:
            if (f.dlc < 4) return;
            c.motor_error = xd::getU32(f.data);
            c.details = true;
            break;
        case xd::kGetEncoderError:
            if (f.dlc < 4) return;
            c.encoder_error = xd::getU32(f.data);
            break;
        default:
            break;
    }
}

void Relay::pushXd(const xd::Frame& f) {
    if (xdq_len_ >= kXdq) {
        xd_dropped_++;
        return;
    }
    xdq_[(xdq_head_ + xdq_len_) % kXdq] = f;
    xdq_len_++;
}

const xd::Frame* Relay::peekXd() const { return xdq_len_ ? &xdq_[xdq_head_] : nullptr; }

void Relay::dropXd() {
    if (!xdq_len_) return;
    xdq_head_ = (uint8_t)((xdq_head_ + 1) % kXdq);
    xdq_len_--;
}

// ---- ch ごとの状態 ----

void Relay::setLink(int i, Link l, uint32_t now) {
    ch_[i].link = l;
    ch_[i].link_at = now;
    ch_[i].retry_at = now - kRetryUs;  // 次の step ですぐ送る
}

void Relay::enterFault(int i, Why why, uint32_t now) {
    Channel& c = ch_[i];
    // 待ち行列に残っているこの node 宛ての指令を捨てる（Estop より先に出ないように）
    uint8_t node = cfg_.xd_node[i];
    uint8_t n = xdq_len_;
    uint8_t keep = 0;
    xd::Frame tmp[kXdq];
    for (uint8_t k = 0; k < n; k++) {
        const xd::Frame& f = xdq_[(xdq_head_ + k) % kXdq];
        if (xd::nodeOf(f.id) != node) tmp[keep++] = f;
    }
    xdq_head_ = 0;
    xdq_len_ = keep;
    for (uint8_t k = 0; k < keep; k++) xdq_[k] = tmp[k];

    setLink(i, Link::Fault, now);
    c.why = why;
    c.fault_axis = why == Why::AxisError ? c.axis_error : 0;
    // Heartbeat が途絶えても送る（ノイズで一部が通らないだけなら、届けば止まる。2026-10-21 実機）
    c.estop_pending = true;
    c.details_pending = true;
    c.details = false;
    c.motor_error = c.encoder_error = 0;
    c.estop_release = false;
}

void Relay::releaseFaults() {
    for (int i = 0; i < kCh; i++) {
        const Channel& c = ch_[i];
        if (c.link != Link::Fault) continue;
        bool hb_ok = !elapsed(last_tick_, c.hb_at, kHeartbeatTimeoutUs);
        if (!hb_ok || c.state == (uint8_t)xd::State::ClosedLoop) continue;  // 止まったと確かめられない
        release(i, last_tick_);
    }
}

// Fault を解いて Idle に戻す。XDrive のエラーを消し、次の Heartbeat を見てから回す
void Relay::release(int i, uint32_t now) {
    Channel& c = ch_[i];
    pushXd(xd::make(cfg_.xd_node[i], xd::kClearErrors, 0));
    c.why = Why::None;
    c.details_pending = c.estop_pending = false;
    c.details = false;
    c.motor_error = c.encoder_error = 0;
    c.fault_axis = 0;
    c.wait_hb = true;
    setLink(i, Link::Idle, now);
}

// 主制御に解除を求めずに戻してよいか（原因と、今の XDrive のエラーの両方で決める）
bool Relay::recoverable(const Channel& c, uint32_t now) const {
    switch (c.why) {
        case Why::HeartbeatLost:   // 電源・CAN が切れて戻った
        case Why::LeftClosedLoop:  // エラー無しで外れた（瞬断で再起動した）
            break;
        case Why::AxisError:  // 低電圧・制御周期の遅れ・前の Estop の跡だけ
            if (c.fault_axis & ~kAxisSoft) return false;
            break;
        default:  // EnableTimeout（校正など）は人が見る
            return false;
    }
    if (c.axis_error & ~kAxisBoot) return false;
    // MOTOR_FAILED の中身が分かるまで待つ。届かなければ戻さない
    if ((c.axis_error | c.fault_axis) & xd::kErrMotorFailed) {
        if (!c.details || (c.motor_error & ~kMotorSoft)) return false;
    }
    return elapsed(now, c.hb_since, kRecoverHoldUs) && elapsed(now, c.link_at, kRecoverHoldUs);
}

void Relay::tick(uint32_t now) {
    uint32_t dt = now - last_tick_;
    last_tick_ = now;
    mas_can_node_tick(&node_, now);
    if (last_estop_ && !node_.estop) releaseFaults();  // E-stop の解除
    last_estop_ = node_.estop;
    for (int i = 0; i < kCh; i++) {
        step(i, now, dt);
        measure(i, now);
    }
    node_.fault = (node_.fault & 0xFFu) | faultDetail(now);
}

void Relay::step(int i, uint32_t now, uint32_t dt) {
    Channel& c = ch_[i];
    const uint8_t node = cfg_.xd_node[i];
    if (node > 63 || c.link == Link::Unseen) return;  // 使わない ch / まだ見えない XDrive には送らない

    const bool hb_ok = !elapsed(now, c.hb_at, kHeartbeatTimeoutUs);
    const bool closed = c.state == (uint8_t)xd::State::ClosedLoop;
    const bool hb_new = after(c.hb_at, c.link_at);  // 今の link になった後の Heartbeat か

    if (c.link != Link::Fault && !hb_ok) enterFault(i, Why::HeartbeatLost, now);

    if (c.link == Link::Fault) {
        if (c.estop_pending) {
            pushXd(xd::make(node, xd::kEstop, 0));
            c.estop_pending = false;
        }
        // XDrive が Closed loop のままなら黙る（XDrive の watchdog を切らせる）
        if (!hb_ok || closed || !hb_new) return;
        if (c.details_pending) {
            pushXd(xd::request(node, xd::kGetMotorError));
            pushXd(xd::request(node, xd::kGetEncoderError));
            c.details_pending = false;
        }
        if (recoverable(c, now)) {
            release(i, now);
            return;
        }
    } else {
        // ---- 主バスの指令から、欲しい状態を決める ----
        const mas_can_md_settings_t* set = mas_can_md_settings(&node_, (uint8_t)i);
        mas_can_md_output_t out = mas_can_md_output(&node_, (uint8_t)i);
        const float gear = set->gear_ratio;
        bool run = false, brake = false;
        xd::Control want = c.control;
        float goal = 0;
        switch (out.mode) {
            case MAS_CAN_MD_OUT_SPEED: {
                run = true;
                want = xd::Control::Velocity;
                float rpm = out.value;
                if (set->max_speed_rpm > 0) rpm = clampf(rpm, set->max_speed_rpm);
                goal = clampf(rpm / 60.0f * gear, cfg_.vel_limit);
                break;
            }
            case MAS_CAN_MD_OUT_CURRENT: {
                run = true;
                want = xd::Control::Torque;
                float lim = (float)set->current_limit_ma / 1000.0f * cfg_.kt;
                goal = out.value / 1000.0f * cfg_.kt;
                c.torque_clamped = goal > lim || goal < -lim;
                goal = clampf(goal, lim);
                break;
            }
            case MAS_CAN_MD_OUT_DUTY:  // XDrive に duty は無い。こなせない指令（measure で bit 4）
            case MAS_CAN_MD_OUT_STOP:
                brake = out.mode == MAS_CAN_MD_OUT_STOP ? out.brake : set->stop_brake;
                break;
        }
        if (out.mode != MAS_CAN_MD_OUT_CURRENT) c.torque_clamped = false;

        // ブレーキ ＝ 回っているなら速度 0 で保持。止まっている（Idle）なら Idle のまま
        if (brake && (c.link == Link::Running)) {
            run = true;
            want = xd::Control::Velocity;
            goal = 0;
            if (node_.estop && !c.estop_release) {  // E-stop は止まったら Idle にする
                c.estop_release = true;
                c.brake_since = now;
            }
            if (c.estop_release &&
                (fabsf(c.vel) < kStoppedVel || elapsed(now, c.brake_since, kEstopBrakeUs)))
                run = false;
        }
        if (!node_.estop) c.estop_release = false;
        if (c.estop_release && c.link != Link::Running) run = false;

        // ---- 状態の移り変わり ----
        switch (c.link) {
            case Link::Idle:
            case Link::Stopping:
                if (c.link == Link::Stopping && hb_new && !closed) setLink(i, Link::Idle, now);
                if (!run || c.wait_hb) break;
                if (!elapsed(now, c.hb_since, kBootSettleUs)) break;  // 起きたばかりの XDrive は回さない
                // XDrive の起動直後は、絶対値 SPI の最初の読み出しが失敗して ENCODER_FAILED が立っている
                // （ABS_SPI_COM_FAIL。2026-10-27 に 2 台で確かめた）。1 度だけ消して、次の Heartbeat で見直す
                if ((c.axis_error & ~xd::kErrWatchdog) == xd::kErrEncoderFailed && !c.enc_cleared) {
                    pushXd(xd::make(node, xd::kClearErrors, 0));
                    c.enc_cleared = true;
                    c.wait_hb = true;
                    break;
                }
                if (c.axis_error & ~xd::kErrWatchdog) {
                    enterFault(i, Why::AxisError, now);
                    return;
                }
                c.control = want;
                c.target = want == xd::Control::Velocity ? c.vel : 0;
                setLink(i, Link::Enabling, now);
                break;
            case Link::Enabling:
                if (!run) {
                    setLink(i, Link::Stopping, now);
                } else if (hb_new && (c.axis_error & ~xd::kErrWatchdog)) {
                    enterFault(i, Why::AxisError, now);
                    return;
                } else if (hb_new && closed) {
                    setLink(i, Link::Running, now);
                    c.enc_cleared = false;
                    c.cmd_at = now - period_us_;
                } else if (elapsed(now, c.link_at, kEnableTimeoutUs)) {
                    enterFault(i, Why::EnableTimeout, now);
                    return;
                }
                break;
            case Link::Running:
                if (c.axis_error) {
                    enterFault(i, Why::AxisError, now);
                    return;
                }
                if (hb_new && !closed) {
                    enterFault(i, Why::LeftClosedLoop, now);
                    return;
                }
                if (!run) setLink(i, Link::Stopping, now);
                break;
            default:
                break;
        }

        // ---- 指令の値（M-29: 最大加速度の中で目標へ向かう。M-25: 動き出しで起点を戻す） ----
        if (out.reset_ctrl || want != c.control)
            c.target = want == xd::Control::Velocity ? c.vel : 0;
        // 設定 46 が 0 なら cfg_.accel（ビルドで決めた値）を使う
        const float accel = set->max_accel_rpm_s > 0 ? set->max_accel_rpm_s / 60.0f * gear : cfg_.accel;
        c.ramping = out.mode == MAS_CAN_MD_OUT_SPEED && accel > 0;
        if (c.ramping) {
            // Closed loop に入るまでは進めない（入ってから最初の指令で目標が飛ばないように）
            if (c.link == Link::Running) c.target += clampf(goal - c.target, accel * (float)dt * 1e-6f);
        } else {
            c.target = goal;
        }

        // ---- 送る ----
        if (c.link == Link::Enabling && elapsed(now, c.retry_at, kRetryUs)) {
            // 餌 → エラーを消す → モードと指令 → Closed loop、を続けて送る
            if (c.axis_error) pushXd(xd::make(node, xd::kClearErrors, 0));
            pushXd(xd::setModes(node, want));
            pushXd(want == xd::Control::Velocity ? xd::inputVel(node, c.target)
                                                 : xd::inputTorque(node, c.target));
            c.sent_target = c.target;
            pushXd(xd::setState(node, xd::State::ClosedLoop));
            c.control = want;
            c.retry_at = now;
        }
        if (c.link == Link::Running) {
            if (want != c.control) {
                pushXd(xd::setModes(node, want));
                c.control = want;
                c.cmd_at = now - period_us_;
            }
            if (elapsed(now, c.cmd_at, period_us_)) {
                if (c.control == xd::Control::Velocity) {
                    pushXd(xd::inputVel(node, c.target, torqueFf(c, now, cfg_)));
                    c.sent_target = c.target;
                } else {
                    pushXd(xd::inputTorque(node, c.target));
                }
                c.cmd_at = now;
            }
        }
        if (c.link == Link::Stopping && elapsed(now, c.retry_at, kRetryUs)) {
            pushXd(xd::setState(node, xd::State::Idle));
            c.retry_at = now;
        }
    }

    // ---- 問い合わせ（XDrive の watchdog の餌も兼ねる） ----
    if (elapsed(now, c.est_at, period_us_)) {
        pushXd(xd::request(node, xd::kGetEncoderEstimates));
        c.est_at = now;
    }
    if (elapsed(now, c.iq_at, 2 * period_us_)) {
        pushXd(xd::request(node, xd::kGetIq));
        c.iq_at = now;
    }
}

void Relay::measure(int i, uint32_t now) {
    Channel& c = ch_[i];
    const mas_can_md_settings_t* set = mas_can_md_settings(&node_, (uint8_t)i);
    const float gear = set->gear_ratio;
    const bool hb_ok = c.link != Link::Unseen && !elapsed(now, c.hb_at, kHeartbeatTimeoutUs);

    mas_can_md_measure_t m = {};
    m.speed_rpm = c.vel * 60.0f / gear;
    float rev = fmodf((c.pos - c.pos0) / gear, 256.0f);  // 24 bit ＝ 出力軸 256 回転で折り返す
    if (rev < 0) rev += 256.0f;
    m.position = (uint32_t)(rev * 65536.0f) & 0xFFFFFFu;
    // XDrive は Closed loop でない間 Iq を更新しない（最後の値が残る。2026-10-27 に USB で確かめた）
    const bool driving = hb_ok && c.state == (uint8_t)xd::State::ClosedLoop;
    m.current_ma = driving ? c.iq * 1000.0f : 0;
    float lim_a = (float)set->current_limit_ma / 1000.0f;
    m.current_limited =
        c.torque_clamped || (driving && lim_a > 0 && fabsf(c.iq_set) >= 0.98f * lim_a);
    const mas_can_node_md_t& md = node_.md[i];
    bool duty = md.commanded && md.mode == MAS_CAN_MD_CMD_DUTY && !node_.estop;
    m.protecting = c.link == Link::Fault || !hb_ok || duty;
    mas_can_md_set_measure(&node_, (uint8_t)i, &m);

}

// 異常の詳細の bit 8〜31 は「今 Fault の ch の原因」。戻れば下ろす（主制御のライブラリは FAULT の bit が
// 消えると手元の詳細も 0 にする）。bit 0〜7（再起動など）は node のもの
uint32_t Relay::faultDetail(uint32_t now) const {
    uint32_t bits = 0;
    for (const Channel& c : ch_) {
        if (c.link != Link::Fault) continue;
        uint32_t b = faultBits(c);
        // AxisError は motor / encoder のエラーが届けば表に当たるかもしれないので、少し待つ
        if (!b && (c.why != Why::AxisError || c.details || elapsed(now, c.link_at, kDetailsWaitUs)))
            b = kFaultOther;
        bits |= b;
    }
    return bits;
}

}  // namespace relay
