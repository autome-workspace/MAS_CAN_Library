// relay.h - 中継の中身（ハードウェアに触らない。PC のテストでも同じものを動かす）。
//
// 主バス（MAS CAN v1）では 4 ch の MD（M-4）として振る舞い、下のバス（XDrive、CANSimple）で
// XDrive の axis を ch ごとに 1 個動かす。main.cpp は受けたフレームを渡し、tick() を回し、
// 出てきたフレームを送るだけ。
//
//   relay.onMas(f, now); relay.onXd(f, now);   // 受けたフレームごと
//   relay.tick(now);
//   while ((f = relay.peekMas()) && 送れた) relay.dropMas();   // XDrive 側も同じ
//
// 時刻は µs（uint32_t、一周してよい）。
#pragma once

#include <stdint.h>

#include "mas/can/node.h"
#include "xd.h"

namespace relay {

constexpr int kCh = 4;

struct Config {
    // 主バス（node にそのまま渡す。load / save / restart もここで）
    mas_can_node_config_t node;

    // 下のバス
    uint8_t xd_node[kCh];       // ch ごとの XDrive の node_id
    uint32_t xd_baud;           // XDrive の can.config.baud_rate と同じ値
    float kt;                   // [Nm/A] XDrive の motor.config.torque_constant と同じ値
    float vel_limit;            // [turn/s] モータ軸。XDrive の vel_limit 以下にする
    uint32_t current_limit_ma;  // XDrive の current_lim [mA]。設定 33 はこれで抑える
};

// ch ごとの XDrive の様子
enum class Link : uint8_t {
    Unseen,    // まだ Heartbeat を見ていない
    Idle,      // Idle（止めている）
    Enabling,  // Closed loop を要求している
    Running,   // Closed loop
    Stopping,  // Idle を要求している
    Fault,     // 止めた。直せる原因なら自分で戻す。ほかは解除（設定 5 の書き込み / E-stop の解除）を待つ
};

enum class Why : uint8_t {
    None,
    HeartbeatLost,   // 見えていた XDrive が見えなくなった
    AxisError,       // XDrive がエラーを出した
    LeftClosedLoop,  // 回している最中に Closed loop から外れた
    EnableTimeout,   // Closed loop に入らない（校正が済んでいない等）
};

const char* name(Link l);
const char* name(Why w);

struct Channel {
    Link link = Link::Unseen;
    Why why = Why::None;

    // XDrive から届いた値
    uint32_t hb_at = 0;
    uint32_t hb_since = 0;  // Heartbeat が途切れずに届き始めた時刻
    uint32_t axis_error = 0;
    uint8_t state = 0;
    bool est_seen = false;
    float pos = 0, vel = 0;  // [turn] [turn/s] モータ軸
    float pos0 = 0;          // 起動して最初に届いた位置（MAS の位置はここを 0 にする。M-2）
    float iq_set = 0, iq = 0;
    uint32_t motor_error = 0, encoder_error = 0;
    bool details = false;  // Fault の後に motor / encoder のエラーを取れたか

    // こちらの指令
    xd::Control control = xd::Control::Velocity;
    float target = 0;  // 加速度の制限の後の値 [turn/s] または [Nm]
    bool torque_clamped = false;
    bool estop_release = false;  // E-stop で速度 0 にした後、止まったら Idle にする
    uint32_t brake_since = 0;
    bool wait_hb = false;  // エラーを消した後。次の Heartbeat を見るまで回さない（古いエラーで断らない）
    bool enc_cleared = false;  // 起動直後のエンコーダのエラーを 1 度消した（Running になったら戻す）

    // 時刻
    uint32_t link_at = 0;  // link が今の値になった時刻
    uint32_t retry_at = 0, cmd_at = 0, est_at = 0, iq_at = 0;
    uint32_t est_rx_at = 0;  // 位置・速度が届いた時刻
    bool estop_pending = false;    // Fault になったので XDrive に Estop を送る
    bool details_pending = false;  // 止まったのを確かめたら motor / encoder のエラーを問い合わせる
    uint32_t fault_axis = 0;       // Fault に入ったときの axis_error（原因で直せるかを決める）
};

class Relay {
public:
    void begin(const Config& cfg, uint32_t now);
    void ready(uint32_t now);  // 起動の知らせを出す（両方のバスを開いた後）

    void onMas(const mas_can_frame_t& f, uint32_t now);
    void onXd(const xd::Frame& f, uint32_t now);
    void tick(uint32_t now);

    const mas_can_frame_t* peekMas();
    void dropMas();
    const xd::Frame* peekXd() const;
    void dropXd();

    const Channel& channel(int i) const { return ch_[i]; }
    const mas_can_node_t& node() const { return node_; }
    uint32_t periodUs() const { return period_us_; }
    uint32_t xdDropped() const { return xd_dropped_; }

private:
    void step(int i, uint32_t now, uint32_t dt);
    void enterFault(int i, Why why, uint32_t now);
    void releaseFaults();
    void release(int i, uint32_t now);
    bool recoverable(const Channel& c, uint32_t now) const;
    void setLink(int i, Link l, uint32_t now);
    void measure(int i, uint32_t now);
    uint32_t faultDetail(uint32_t now) const;
    bool intercept(const mas_can_frame_t& f);
    void pushXd(const xd::Frame& f);
    void pushMasExtra(const mas_can_frame_t& f);
    int localOfNode(uint8_t node) const;

    Config cfg_{};
    mas_can_node_t node_{};
    Channel ch_[kCh];
    uint32_t period_us_ = 20000;
    uint32_t last_tick_ = 0;
    bool last_estop_ = false;

    // 主バスへ出すもの: node の待ち行列を先に、ここで作った応答を後に送る
    static constexpr int kMasExtra = 4;
    mas_can_frame_t mas_extra_[kMasExtra];
    uint8_t mas_extra_head_ = 0, mas_extra_len_ = 0;
    mas_can_frame_t fb_patched_;  // FB を書き換えて送るときの置き場
    bool from_extra_ = false;

    static constexpr int kXdq = 48;
    xd::Frame xdq_[kXdq];
    uint8_t xdq_head_ = 0, xdq_len_ = 0;
    uint32_t xd_dropped_ = 0;
};

}  // namespace relay
