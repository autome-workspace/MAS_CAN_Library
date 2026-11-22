// 中継の中身（relay.cpp）を PC で動かすテスト。偽の XDrive と、主バスに流すフレームで確かめる。
//   make -C test MAS=~/dev/MAS_CAN_Library
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"
#include "relay.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

// ---- 偽の XDrive（fw 0.5.1 の振る舞いのうち、中継が頼るところ） ----
struct FakeXd {
    uint8_t node;
    bool present = true;
    bool calibrated = true;
    bool enc_broken = false;  // エンコーダが本当に読めない（Closed loop の要求で ENCODER_FAILED）
    uint32_t boot_at = 0, settle_us = 0;  // 起きてから settle_us の間に Closed loop を求められると落ちる
    bool watchdog = true;  // axis.config.enable_watchdog
    uint32_t wd_timeout_us = 200000;
    uint8_t state = 1;
    uint32_t error = 0;
    uint32_t motor_error = 0;
    uint8_t control = 2, input_mode = 1;
    float input_vel = 0, input_torque = 0, torque_ff = 0;
    float pos = 3.25f, vel = 0;  // 絶対値エンコーダなので起動時も 0 でない
    uint32_t fed_at = 0, hb_at = 0;
    int n_estop = 0, n_clear = 0, n_modes = 0, n_closed = 0, n_rx = 0;

    void onFrame(const xd::Frame& f, uint32_t now, std::vector<xd::Frame>& out) {
        if (!present || xd::nodeOf(f.id) != node) return;
        n_rx++;
        fed_at = now;  // どのフレームでも餌になる（CANSimple の doCommand の先頭）
        switch (xd::cmdOf(f.id)) {
            case xd::kEstop: n_estop++; error |= xd::kErrEstop; state = 1; break;
            case xd::kSetAxisState: {
                uint32_t s = xd::getU32(f.data);
                if (s == 8) {
                    n_closed++;
                    if (!calibrated) error |= 0x1;
                    else if ((uint32_t)(now - boot_at) < settle_us) {
                        error |= xd::kErrControllerFailed;
                        motor_error |= xd::kMotorControlDeadlineMissed;
                    }
                    else if (enc_broken) error |= xd::kErrEncoderFailed;
                    else if (error == 0) state = 8;
                } else if (s == 1) {
                    state = 1;
                }
                break;
            }
            case xd::kSetControllerModes:
                n_modes++;
                control = (uint8_t)xd::getU32(f.data);
                input_mode = (uint8_t)xd::getU32(f.data + 4);
                break;
            case xd::kSetInputVel:
                input_vel = xd::getF32(f.data);
                torque_ff = xd::getF32(f.data + 4);
                break;
            case xd::kSetInputTorque: input_torque = xd::getF32(f.data); break;
            case xd::kClearErrors: n_clear++; error = 0; motor_error = 0; break;
            case xd::kGetEncoderEstimates: {
                xd::Frame r = xd::make(node, xd::kGetEncoderEstimates, 8);
                xd::putF32(r.data, pos);
                xd::putF32(r.data + 4, vel);
                out.push_back(r);
                break;
            }
            case xd::kGetIq: {
                xd::Frame r = xd::make(node, xd::kGetIq, 8);
                xd::putF32(r.data, input_torque / 0.014768f);
                xd::putF32(r.data + 4, input_torque / 0.014768f);
                out.push_back(r);
                break;
            }
            case xd::kGetMotorError: {
                xd::Frame r = xd::make(node, xd::kGetMotorError, 8);
                xd::putU32(r.data, motor_error);
                out.push_back(r);
                break;
            }
            case xd::kGetEncoderError: out.push_back(xd::make(node, xd::kGetEncoderError, 8)); break;
            default: break;
        }
    }

    void tick(uint32_t now, uint32_t dt, std::vector<xd::Frame>& out) {
        if (!present) return;
        if (watchdog && (uint32_t)(now - fed_at) > wd_timeout_us) {
            error |= xd::kErrWatchdog;  // Idle でも立つ（2026-10-20 に USB で確かめた）
            state = 1;
        }
        if (state == 8 && control == 2) vel += (input_vel - vel) * 0.05f;
        if (state == 8 && control == 1) vel += input_torque * 0.5f;
        if (state != 8) vel *= 0.98f;
        pos += vel * (float)dt * 1e-6f;
        if ((uint32_t)(now - hb_at) >= 100000) {
            hb_at = now;
            xd::Frame h = xd::make(node, xd::kHeartbeat, 8);
            xd::putU32(h.data, error);
            h.data[4] = state;
            out.push_back(h);
        }
    }
};

// ---- 試験台 ----
static unsigned char flash[sizeof(mas_can_node_saved_t)];
static bool flashLoad(void*, void* buf, size_t len) { memcpy(buf, flash, len); return true; }
static bool flashSave(void*, const void* buf, size_t len) { memcpy(flash, buf, len); return true; }

struct Bench {
    relay::Relay r;
    FakeXd xd[relay::kCh];
    uint32_t now = 1000;
    std::vector<mas_can_frame_t> mas_out;
    std::vector<xd::Frame> xd_sent;
    int xd_per_tick_max = 1000;  // 1 ms に送れる XDrive のフレームの数

    explicit Bench(uint32_t baud = 250000, int n_xd = 1, float accel = 0, float inertia = 0, float friction = 0) {
        memset(flash, 0xFF, sizeof(flash));
        relay::Config cfg = {};
        cfg.node.uid = 0x0102030405060708ull;
        cfg.node.md_first_ch = 4;
        cfg.node.load = flashLoad;
        cfg.node.save = flashSave;
        for (int i = 0; i < relay::kCh; i++) {
            cfg.xd_node[i] = i < n_xd ? (uint8_t)(2 + i) : 0xFF;
            xd[i].node = (uint8_t)(2 + i);
            xd[i].present = i < n_xd;
        }
        cfg.xd_baud = baud;
        cfg.kt = 0.014768f;
        cfg.vel_limit = 18.0f;
        cfg.current_limit_ma = 12000;
        cfg.accel = accel;
        cfg.inertia = inertia;
        cfg.friction = friction;
        r.begin(cfg, now);
        r.ready(now);
    }

    void run(int ms) {
        for (int k = 0; k < ms; k++) {
            now += 1000;
            std::vector<xd::Frame> back;
            for (auto& x : xd) x.tick(now, 1000, back);
            for (auto& f : back) r.onXd(f, now);
            r.tick(now);
            const mas_can_frame_t* f;
            while ((f = r.peekMas())) {
                mas_out.push_back(*f);
                r.dropMas();
            }
            const xd::Frame* g;
            int sent = 0;
            back.clear();
            while (sent < xd_per_tick_max && (g = r.peekXd())) {
                xd_sent.push_back(*g);
                for (auto& x : xd) x.onFrame(*g, now, back);
                r.dropXd();
                sent++;
            }
            for (auto& f : back) r.onXd(f, now);
        }
    }

    void mas(const mas_can_frame_t& f) { r.onMas(f, now); }

    void cmd(mas_can_md_cmd_t kind, uint8_t ch, int16_t v) {
        mas_can_frame_t f;
        mas_can_md_cmd_init(&f, kind, ch);
        mas_can_md_cmd_set(&f, ch, v);
        mas(f);
    }
    // 指令を period_ms ごとに ms の間送り続ける
    void hold(mas_can_md_cmd_t kind, uint8_t ch, int16_t v, int ms, int period_ms = 10) {
        for (int t = 0; t < ms; t += period_ms) {
            cmd(kind, ch, v);
            run(period_ms);
        }
    }

    mas_can_setting_t setting(uint8_t ch, mas_can_op_t op, uint8_t item, const uint8_t v[4]) {
        mas_can_setting_t s = {};
        s.kind = MAS_CAN_KIND_MD;
        s.ch = ch;
        s.op = op;
        s.item = item;
        if (v) memcpy(s.value, v, 4);
        mas_can_frame_t f;
        mas_can_setting_encode(&s, &f);
        mas_out.clear();
        mas(f);
        run(2);
        for (auto& o : mas_out) {
            mas_can_setting_t res;
            if (mas_can_setting_decode(&o, &res) && res.response && res.item == item) return res;
        }
        mas_can_setting_t none = {};
        none.result = 0xFF;
        return none;
    }
    uint8_t writeF(uint8_t ch, uint8_t item, float x) {
        uint8_t v[4];
        mas_can_value_set_f32(v, x);
        return setting(ch, MAS_CAN_OP_WRITE, item, v).result;
    }
    uint8_t writeU(uint8_t ch, uint8_t item, uint32_t x) {
        uint8_t v[4];
        mas_can_value_set_u32(v, x);
        return setting(ch, MAS_CAN_OP_WRITE, item, v).result;
    }

    // 最後の FB
    bool fb(uint8_t want_ch, mas_can_md_fb_t* out) {
        for (auto it = mas_out.rbegin(); it != mas_out.rend(); ++it) {
            uint8_t ch;
            if (mas_can_md_fb_decode(&*it, &ch, out) && ch == want_ch) return true;
        }
        return false;
    }
    int countXd(uint8_t node, uint8_t cmd) const {
        int n = 0;
        for (auto& f : xd_sent) n += xd::nodeOf(f.id) == node && xd::cmdOf(f.id) == cmd;
        return n;
    }
};

// ---- テスト ----

static void testBootIdle() {
    Bench b;
    b.run(500);
    const relay::Channel& c = b.r.channel(0);
    CHECK(c.link == relay::Link::Idle);
    CHECK(b.xd[0].state == 1);
    CHECK((b.xd[0].error & xd::kErrWatchdog) == 0);  // Idle の問い合わせが餌になっている
    CHECK(b.countXd(2, xd::kSetAxisState) == 0);     // 指令が無ければ何も要求しない
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(!(fb.status & MAS_CAN_MD_ST_SPEED_INVALID));
    CHECK(fb.status & MAS_CAN_MD_ST_STOPPED);
    CHECK(fb.position == 0);  // 起動時の位置を 0 にする（M-2）
    // XDrive の無い ch は速度・電流が無効
    CHECK(b.fb(5, &fb));
    CHECK((fb.status & (MAS_CAN_MD_ST_SPEED_INVALID | MAS_CAN_MD_ST_CURRENT_INVALID)) ==
          (MAS_CAN_MD_ST_SPEED_INVALID | MAS_CAN_MD_ST_CURRENT_INVALID));
    CHECK(b.countXd(3, xd::kGetEncoderEstimates) == 0);
}

static void testSpeed() {
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    CHECK(b.writeF(4, 37, 10.0f) == MAS_CAN_RES_OK);  // 減速比 10
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 60, 1000);        // 出力軸 60 rpm ＝ モータ軸 10 turn/s
    const relay::Channel& c = b.r.channel(0);
    CHECK(c.link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);
    CHECK(fabsf(b.xd[0].input_vel - 10.0f) < 1e-3f);
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(abs(fb.speed - 60) <= 1);
    CHECK(!(fb.status & MAS_CAN_MD_ST_STOPPED));
    CHECK(((fb.status >> MAS_CAN_MD_ST_MODE_SHIFT) & 3) == MAS_CAN_MD_CMD_SPEED);

    // 反転（32）は node が指令と FB の両方に掛ける
    CHECK(b.writeU(4, 32, 1) == MAS_CAN_RES_OK);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 60, 1000);
    CHECK(fabsf(b.xd[0].input_vel + 10.0f) < 1e-3f);
    CHECK(b.fb(4, &fb));
    CHECK(abs(fb.speed - 60) <= 1);

    // 指令が途絶えたら 100 ms で止める。止め方の既定は空転 ＝ Idle
    b.run(400);
    CHECK(c.link == relay::Link::Idle);
    CHECK(b.xd[0].state == 1);
    CHECK((b.xd[0].error & xd::kErrWatchdog) == 0);
}

static void testLimits() {
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    CHECK(b.writeF(4, 45, 120.0f) == MAS_CAN_RES_OK);  // 最高速度 120 rpm ＝ 2 turn/s
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 600, 300);
    CHECK(fabsf(b.xd[0].input_vel - 2.0f) < 1e-3f);
    CHECK(b.xd[0].input_mode == 1);  // 速度も PASSTHROUGH（ランプと FF は中継機が作る）
    CHECK(b.xd[0].torque_ff == 0);   // inertia 0 なら FF なし
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 0, 300);

    // 最大加速度 600 rpm/s ＝ 10 turn/s²。0 → 2 turn/s に 0.2 s
    CHECK(b.writeF(4, 46, 600.0f) == MAS_CAN_RES_OK);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 120, 100);
    float mid = b.xd[0].input_vel;
    CHECK(mid > 0.6f && mid < 1.4f);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 120, 300);
    CHECK(fabsf(b.xd[0].input_vel - 2.0f) < 1e-3f);

    // 電流: 3000 mA ＝ 0.0443 Nm。上限 33 を 1000 mA にすると 0.0148 Nm で抑え、bit 3
    b.hold(MAS_CAN_MD_CMD_CURRENT, 4, 3000, 200);
    CHECK(b.xd[0].control == 1);
    CHECK(b.xd[0].input_mode == 1);  // トルクは PASSTHROUGH
    CHECK(fabsf(b.xd[0].input_torque - 3.0f * 0.014768f) < 1e-4f);
    CHECK(b.r.channel(0).link == relay::Link::Running);  // 種類を変えても Closed loop のまま
    CHECK(b.writeU(4, 33, 1000) == MAS_CAN_RES_OK);
    b.hold(MAS_CAN_MD_CMD_CURRENT, 4, 3000, 200);
    CHECK(fabsf(b.xd[0].input_torque - 0.014768f) < 1e-4f);
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(fb.status & MAS_CAN_MD_ST_CURRENT_LIMIT);
    // 33 は XDrive の current_lim（12 A）で抑える
    uint8_t v[4];
    mas_can_value_set_u32(v, 50000);
    mas_can_setting_t r = b.setting(4, MAS_CAN_OP_WRITE, 33, v);
    CHECK(r.result == MAS_CAN_RES_OK && mas_can_value_u32(r.value) == 12000);
}

// 加速度の制限（ビルドの値、設定 46）と慣性の FF
static void testFeedforward() {
    Bench b(250000, 1, 10.0f, 0.0145f);  // 10 turn/s²、0.0145 Nm/(turn/s²) ＝ FF 0.145 Nm
    b.run(1200);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 0, 300);  // 速度 0 で Closed loop に入れておく
    CHECK(b.r.channel(0).link == relay::Link::Running);
    CHECK(b.xd[0].torque_ff == 0);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 120, 100);  // 2 turn/s。0.2 s で届く
    CHECK(b.xd[0].input_mode == 1);
    CHECK(b.xd[0].input_vel > 0.6f && b.xd[0].input_vel < 1.4f);
    CHECK(fabsf(b.xd[0].torque_ff - 0.145f) < 0.015f);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 120, 300);
    CHECK(fabsf(b.xd[0].input_vel - 2.0f) < 1e-3f);
    CHECK(b.xd[0].torque_ff == 0);  // 届いたら FF は 0
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 0, 100);  // 減速は負の FF
    CHECK(fabsf(b.xd[0].torque_ff + 0.145f) < 0.015f);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 0, 300);
    CHECK(fabsf(b.xd[0].input_vel) < 1e-3f);
    CHECK(b.xd[0].torque_ff == 0);

    // 設定 46 があればそちらを使う: 60 rpm/s ＝ 1 turn/s² ＝ FF 0.0145 Nm
    CHECK(b.writeF(4, 46, 60.0f) == MAS_CAN_RES_OK);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 120, 500);
    CHECK(b.xd[0].input_vel > 0.3f && b.xd[0].input_vel < 0.7f);
    CHECK(fabsf(b.xd[0].torque_ff - 0.0145f) < 0.002f);

    // Idle から動き出すとき: Closed loop に入る前に目標を進めない（最初の指令が飛ばない）
    b.run(400);
    CHECK(b.r.channel(0).link == relay::Link::Idle);
    CHECK(b.writeF(4, 46, 0.0f) == MAS_CAN_RES_OK);  // ビルドの値（10 turn/s²）に戻す
    float first = -1;
    for (int t = 0; t < 400 && first < 0; t += 10) {
        b.cmd(MAS_CAN_MD_CMD_SPEED, 4, 120);
        b.run(10);
        if (b.r.channel(0).link == relay::Link::Running && b.xd[0].input_vel > 0) first = b.xd[0].input_vel;
    }
    CHECK(first > 0 && first < 0.3f);
    CHECK(b.xd[0].torque_ff < 0.16f);

    // 電流の指令には FF を載せない（トルクの指令そのもの）
    b.hold(MAS_CAN_MD_CMD_CURRENT, 4, 1000, 200);
    CHECK(b.xd[0].control == 1);
    CHECK(fabsf(b.xd[0].input_torque - 0.014768f) < 1e-4f);
}

// 摩擦の FF: 目標の向きに一定。0 の近くは比例、0 なら 0。慣性の FF に足す
static void testFriction() {
    Bench b(250000, 1, 10.0f, 0.0145f, 0.04f);
    b.run(1200);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 0, 300);
    CHECK(b.r.channel(0).link == relay::Link::Running);
    CHECK(b.xd[0].torque_ff == 0);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 120, 100);  // 加速中: 0.145 ＋ 0.04
    CHECK(fabsf(b.xd[0].torque_ff - 0.185f) < 0.015f);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 120, 300);  // 一定: 摩擦だけ
    CHECK(fabsf(b.xd[0].torque_ff - 0.04f) < 1e-4f);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, -120, 600);  // 逆向き
    CHECK(fabsf(b.xd[0].torque_ff + 0.04f) < 1e-4f);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 3, 400);  // 3 rpm ＝ 0.05 turn/s: 帯の中なので半分
    CHECK(fabsf(b.xd[0].torque_ff - 0.02f) < 1e-3f);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 0, 300);
    CHECK(b.xd[0].torque_ff == 0);
}

static void testBrakeAndEstop() {
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    CHECK(b.writeU(4, 34, 1) == MAS_CAN_RES_OK);  // 止め方 ＝ ブレーキ
    b.run(500);
    CHECK(b.xd[0].state == 1);  // 回していないなら、ブレーキでも Closed loop に入れない
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    b.run(500);  // 途絶 → ブレーキ ＝ 速度 0 で保持
    CHECK(b.xd[0].state == 8);
    CHECK(b.xd[0].input_vel == 0.0f);
    CHECK(b.r.channel(0).link == relay::Link::Running);

    // E-stop: 速度 0 にして、止まったら Idle
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    mas_can_frame_t e = {};
    e.id = MAS_CAN_ID_ESTOP;
    b.mas(e);
    b.run(20);
    CHECK(b.xd[0].input_vel == 0.0f);
    b.run(1500);
    CHECK(b.xd[0].state == 1);
    CHECK(b.r.channel(0).link == relay::Link::Idle);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 300);  // E-stop の間は動かない
    CHECK(b.xd[0].state == 1);
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(fb.status & MAS_CAN_MD_ST_ESTOP);

    mas_can_frame_t clr;
    mas_can_estop_clear_encode(&clr);
    b.mas(clr);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(b.xd[0].state == 8);
}

static void testXdError() {
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(b.xd[0].state == 8);

    // XDrive が過電流で止まった（自分では戻さない）
    b.xd[0].error = xd::kErrMotorFailed;
    b.xd[0].motor_error = xd::kMotorDcBusOverCurrent;
    b.xd[0].state = 1;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 2000);
    const relay::Channel& c = b.r.channel(0);
    CHECK(c.link == relay::Link::Fault);
    CHECK(c.why == relay::Why::AxisError);
    CHECK(b.xd[0].n_estop == 1);
    CHECK(c.details && c.motor_error == xd::kMotorDcBusOverCurrent);
    CHECK(b.r.node().fault & (1u << 8));
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(fb.status & MAS_CAN_MD_ST_STOPPED);
    CHECK(fb.status & MAS_CAN_MD_ST_FAULT);
    int closed_before = b.xd[0].n_closed;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(b.xd[0].n_closed == closed_before);  // Fault の間は回そうとしない

    // 主制御が異常の詳細を消す → XDrive のエラーも消して、また回る
    uint8_t v[4];
    mas_can_value_set_u32(v, 0xFFFFFFFFu);
    CHECK(b.setting(4, MAS_CAN_OP_WRITE, MAS_CAN_ITEM_FAULT_DETAIL, v).result == MAS_CAN_RES_OK);
    CHECK(b.xd[0].n_clear >= 1);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(c.link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);
    CHECK(b.r.node().fault == 0);
}

static void testUncalibrated() {
    Bench b;
    b.xd[0].calibrated = false;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(b.r.channel(0).link == relay::Link::Fault);
    CHECK(b.r.channel(0).why == relay::Why::AxisError);
}

static void testHeartbeatLost() {
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    CHECK(b.writeU(4, MAS_CAN_ITEM_FAULT_DETAIL, MAS_CAN_FAULT_RESTARTED) == MAS_CAN_RES_OK);  // ライブラリと同じ
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    b.xd[0].present = false;  // 電源が落ちた
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    const relay::Channel& c = b.r.channel(0);
    CHECK(c.link == relay::Link::Fault && c.why == relay::Why::HeartbeatLost);
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(fb.status & MAS_CAN_MD_ST_SPEED_INVALID);
    CHECK(fb.status & MAS_CAN_MD_ST_STOPPED);

    CHECK(b.r.node().fault & (1u << 31));
    CHECK(fb.status & MAS_CAN_MD_ST_FAULT);

    // 電源が戻った（再起動した XDrive には watchdog とエンコーダのエラーが立っている）。
    // 主制御は何もしない。0.5 s 見えていたら中継機が自分で戻し、起きてから 1 s たてば、指令が続いていれば回る
    b.xd[0].present = true;
    b.xd[0].state = 1;
    b.xd[0].error = xd::kErrWatchdog | xd::kErrEncoderFailed;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 300);
    CHECK(c.link == relay::Link::Fault);  // すぐには戻さない
    CHECK(b.xd[0].state == 1);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 600);
    CHECK(c.link == relay::Link::Idle);   // 戻したが、起きたばかりなのでまだ回さない
    CHECK(b.xd[0].state == 1);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 700);
    CHECK(c.link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);
    CHECK((b.r.node().fault & ~MAS_CAN_FAULT_RESTARTED) == 0);  // 異常の詳細も下ろす
    CHECK(b.fb(4, &fb));
    CHECK(!(fb.status & MAS_CAN_MD_ST_FAULT));
}

static void testWatchdogAtBoot() {
    // 中継機より先に XDrive が起きていた: watchdog の 0x800 が立っている
    Bench b;
    b.xd[0].error = xd::kErrWatchdog;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(b.r.channel(0).link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);
}

static void testUnlistedFault() {
    // 表に無い原因（OVERSPEED ＝ CONTROLLER_FAILED）でも、FAULT の bit が立ち、異常の詳細を消せば解ける
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    b.xd[0].error = xd::kErrControllerFailed;
    b.xd[0].state = 1;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 1000);
    const relay::Channel& c = b.r.channel(0);
    CHECK(c.link == relay::Link::Fault && c.why == relay::Why::AxisError);
    CHECK((b.r.node().fault & ~MAS_CAN_FAULT_RESTARTED) == (1u << 31));  // 再起動の bit は起動時から立っている
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(fb.status & MAS_CAN_MD_ST_FAULT);
    // ライブラリの clearFault と同じ: 読んだ詳細をそのまま書く
    CHECK(b.writeU(4, MAS_CAN_ITEM_FAULT_DETAIL, b.r.node().fault) == MAS_CAN_RES_OK);
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(c.link == relay::Link::Running);
    CHECK(b.r.node().fault == 0);

    // OVERSPEED は自分では戻さない（原因が残っていれば回すたびに暴走する）
    b.xd[0].error = xd::kErrControllerFailed;
    b.xd[0].state = 1;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 3000);
    CHECK(c.link == relay::Link::Fault);
    CHECK(b.xd[0].state == 1);
    CHECK(b.r.node().fault & (1u << 31));
}

static void testAutoRecover() {
    // エラー無しで Closed loop から外れた（瞬断で XDrive が再起動した）→ 自分で戻る
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    const relay::Channel& c = b.r.channel(0);
    b.xd[0].state = 1;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 100);
    CHECK(c.link == relay::Link::Fault && c.why == relay::Why::LeftClosedLoop);
    CHECK(b.r.node().fault & (1u << 31));
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 1000);
    CHECK(c.link == relay::Link::Running);
    CHECK((b.r.node().fault & ~MAS_CAN_FAULT_RESTARTED) == 0);

    // 低電圧（電源を切った・電圧が落ちた）→ bit 10 を見せてから自分で戻る
    b.xd[0].error = xd::kErrUnderVoltage;
    b.xd[0].state = 1;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 200);
    CHECK(c.link == relay::Link::Fault);
    CHECK(b.r.node().fault & (1u << 10));
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 1000);
    CHECK(c.link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);

    // 制御周期の遅れ（CONTROL_DEADLINE_MISSED）だけ → 戻る
    b.xd[0].error = xd::kErrMotorFailed | xd::kErrMotorDisarmed;
    b.xd[0].motor_error = xd::kMotorControlDeadlineMissed;
    b.xd[0].state = 1;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 1500);
    CHECK(c.link == relay::Link::Running);

    // 何度起きても、そのたびに自分で戻る（回数の上限は無い）
    for (int k = 0; k < 6; k++) {
        b.xd[0].error = xd::kErrUnderVoltage;
        b.xd[0].state = 1;
        b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 1500);
        CHECK(c.link == relay::Link::Running);
    }
    // CAN が何度抜けても同じ
    for (int k = 0; k < 6; k++) {
        b.xd[0].present = false;
        b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
        CHECK(c.link == relay::Link::Fault);
        b.xd[0].present = true;
        b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 2000);
        CHECK(c.link == relay::Link::Running);
    }
}

static void testBootSettle() {
    // 主制御が指令を出し続けている中で XDrive が起きた。起きてすぐに回しに行かない
    Bench b;
    b.xd[0].present = false;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 300);
    b.xd[0].present = true;
    b.xd[0].boot_at = b.now;
    b.xd[0].settle_us = 500000;
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 2000);
    CHECK(b.r.channel(0).link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);
    CHECK((b.xd[0].error & xd::kErrControllerFailed) == 0);

    // 中継機が起きたとき、XDrive に前の分のエラー（落ちたまま）が残っていた → 1 度消して回す
    Bench k;
    k.xd[0].error = xd::kErrControllerFailed | xd::kErrEstop;
    k.xd[0].motor_error = xd::kMotorControlDeadlineMissed;
    k.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 2000);
    CHECK(k.r.channel(0).link == relay::Link::Running);
}

static void testLeftoverEstop() {
    // 中継機を入れ直した: XDrive に前の中継機の Estop の跡が残っている → 自分で戻して回す
    Bench b;
    b.xd[0].error = xd::kErrEstop;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 1500);
    CHECK(b.r.channel(0).link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);
}

static void testStaleIq() {
    // Closed loop でない XDrive の Iq は古い値。FB の電流は 0、bit 3 も立てない
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_CURRENT, 4, 30000, 300);  // 12 A（current_lim）で抑えられる
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(fb.status & MAS_CAN_MD_ST_CURRENT_LIMIT);
    CHECK(fb.current != 0);
    b.run(500);  // 途絶 → 空転 ＝ Idle。偽の XDrive は Iq を古いまま返し続ける
    CHECK(b.xd[0].state == 1);
    CHECK(b.r.channel(0).iq > 10.0f);
    CHECK(b.fb(4, &fb));
    CHECK(fb.current == 0);
    CHECK(!(fb.status & MAS_CAN_MD_ST_CURRENT_LIMIT));
    CHECK(fb.status & MAS_CAN_MD_ST_STOPPED);
}

static void testEncoderAtBoot() {
    // 起動直後の ENCODER_FAILED（ABS_SPI_COM_FAIL）は 1 度消して回す
    Bench b;
    b.xd[0].error = xd::kErrEncoderFailed | xd::kErrWatchdog;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 500);
    CHECK(b.r.channel(0).link == relay::Link::Running);
    CHECK(b.xd[0].state == 8);
    CHECK((b.r.node().fault & ~MAS_CAN_FAULT_RESTARTED) == 0);

    // 本当に読めないなら、消しても Closed loop の要求でまた立つ → Fault（bit 12）
    Bench k;
    k.xd[0].error = xd::kErrEncoderFailed;
    k.xd[0].enc_broken = true;
    k.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    k.hold(MAS_CAN_MD_CMD_SPEED, 4, 300, 1000);
    CHECK(k.r.channel(0).link == relay::Link::Fault);
    CHECK(k.r.node().fault & (1u << 12));
    CHECK(k.xd[0].state == 1);
}

static void testDutyAndSettings() {
    Bench b;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    b.hold(MAS_CAN_MD_CMD_DUTY, 4, mas_can_md_duty_pack(8000, false), 300);
    CHECK(b.xd[0].state == 1);
    mas_can_md_fb_t fb;
    CHECK(b.fb(4, &fb));
    CHECK(fb.status & MAS_CAN_MD_ST_STOPPED);
    CHECK(((fb.status >> MAS_CAN_MD_ST_MODE_SHIFT) & 3) == MAS_CAN_MD_CMD_DUTY);

    CHECK(b.writeU(4, 38, 1) == MAS_CAN_RES_NOT_REALIZABLE);
    CHECK(b.writeF(4, 39, 1.0f) == MAS_CAN_RES_NOT_REALIZABLE);
    CHECK(b.writeF(7, 41, 1.0f) == MAS_CAN_RES_NOT_REALIZABLE);
    CHECK(b.writeF(8, 39, 1.0f) == 0xFF);  // 受け持たない ch には返さない
    CHECK(b.writeU(4, 35, 50) == MAS_CAN_RES_OK);
    uint8_t v[4] = {};
    CHECK(b.setting(4, MAS_CAN_OP_SAVE, 0, v).result == MAS_CAN_RES_OK);
}

static void testFourAxesLoad() {
    // 4 台、250 kbps。1 ms に送れるのは 2 本ほど（8 byte のフレーム 1 本 ≈ 0.5 ms）
    Bench b(250000, 4);
    b.xd_per_tick_max = 2;
    b.run(1200);  // 起動して XDrive が落ち着くまで（kBootSettleUs）
    for (int t = 0; t < 2000; t += 10) {
        for (uint8_t ch = 4; ch < 8; ch++) b.cmd(MAS_CAN_MD_CMD_SPEED, ch, 300);
        b.run(10);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(b.r.channel(i).link == relay::Link::Running);
        CHECK((b.xd[i].error & xd::kErrWatchdog) == 0);
    }
    CHECK(b.r.xdDropped() == 0);
    // 1 秒あたりのフレームの数（最後の 1 秒）
    size_t before = b.xd_sent.size();
    for (int t = 0; t < 1000; t += 10) {
        for (uint8_t ch = 4; ch < 8; ch++) b.cmd(MAS_CAN_MD_CMD_SPEED, ch, 300);
        b.run(10);
    }
    size_t n = b.xd_sent.size() - before;
    printf("  4 axes @250k: %zu frames/s sent by relay\n", n);
    CHECK(n < 1000);
}

int main() {
    testBootIdle();
    testSpeed();
    testLimits();
    testFeedforward();
    testFriction();
    testBrakeAndEstop();
    testXdError();
    testUncalibrated();
    testHeartbeatLost();
    testWatchdogAtBoot();
    testUnlistedFault();
    testStaleIq();
    testEncoderAtBoot();
    testAutoRecover();
    testLeftoverEstop();
    testBootSettle();
    testDutyAndSettings();
    testFourAxesLoad();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
