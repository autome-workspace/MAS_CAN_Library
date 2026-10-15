// 主制御のライブラリ（Host と取っ手）のテスト。偽のポートで PC で動かす。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "mas/can.h"
#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"

namespace mcan = mas::can;

constexpr mcan::Host kConstantInit{}; // コンストラクタが constexpr であること（API-35）

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

struct FakePort : mcan::Port {
    uint32_t now = 1000000;
    uint32_t tick = 0; // micros() を呼ぶたびに進める量（waitReady の試験用）
    uint32_t overflows = 0;
    int answered_seq = -1; // answerAll が最後に応えた通し番号
    std::deque<mcan::Frame> rx;
    std::vector<mcan::Frame> tx;
    bool send(const mcan::Frame& f) override {
        tx.push_back(f);
        return true;
    }
    bool receive(mcan::Frame& f) override {
        if (rx.empty()) return false;
        f = rx.front();
        rx.pop_front();
        return true;
    }
    uint32_t micros() override { return now += tick; }
    uint32_t rxOverflows() override { return overflows; }

    const mcan::Frame* last(uint16_t id) const {
        for (auto it = tx.rbegin(); it != tx.rend(); ++it)
            if (it->id == id) return &*it;
        return nullptr;
    }
    int count(uint16_t id) const {
        int n = 0;
        for (auto& f : tx) n += f.id == id;
        return n;
    }
};

static int16_t slot(const mcan::Frame& f, int ch) {
    int16_t v = 0;
    mas_can_md_cmd_get(&f, (uint8_t)ch, &v);
    return v;
}

static void step(mcan::Host& host, FakePort& port, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        port.now += 1000;
        host.update();
    }
}

// 設定の要求に応答する。書くは書いた値を返す（C-85）。value_override は読むの値
static void respond(FakePort& port, const mcan::Frame& req, uint8_t result,
                    const uint32_t* read_value = nullptr) {
    mas_can_setting_t s;
    mas_can_setting_decode(&req, &s);
    s.response = true;
    s.result = result;
    if (read_value) mas_can_value_set_u32(s.value, *read_value);
    mcan::Frame f;
    mas_can_setting_encode(&s, &f);
    port.rx.push_back(f);
}

// 待っている設定の要求に次々と成功で応える（同じ要求に 2 度は応えない）
static void answerAll(mcan::Host& host, FakePort& port, int rounds = 20) {
    for (int i = 0; i < rounds; i++) {
        const mcan::Frame* req = port.last(0x400);
        if (req && req->data[7] != port.answered_seq) {
            port.answered_seq = req->data[7];
            respond(port, *req, MAS_CAN_RES_OK);
        }
        step(host, port, 1);
    }
}

static void pushFb(FakePort& port, uint8_t ch, uint8_t status = 0x40, int16_t cur = 0,
                   int16_t spd = 0, uint32_t pos = 0) {
    mas_can_md_fb_t fb = {};
    fb.status = status;
    fb.current = cur;
    fb.speed = spd;
    fb.position = pos;
    mcan::Frame f;
    mas_can_md_fb_encode(&fb, ch, &f);
    port.rx.push_back(f);
}

static void test_ready_and_commands() {
    FakePort port;
    mcan::Host host;
    mcan::Md w[4] = {host.md(0), host.md(1), host.md(2), host.md(3)};
    CHECK(host.begin(port));
    for (int i = 0; i < 4; i++) w[i].setSpeed(10.0f * (i + 1)); // rad/s

    // 基板が見えて刻みを書くまでは止める値（API-43、API-44）
    step(host, port, 10);
    CHECK(!w[0].ready());
    CHECK(port.last(0x218) && slot(*port.last(0x218), 0) == 0);
    CHECK(port.count(0x400) == 0);

    for (int i = 0; i < 4; i++) pushFb(port, (uint8_t)i);
    step(host, port, 1);
    const mcan::Frame* req = port.last(0x400);
    CHECK(req && req->data[1] == 0x40 && (req->data[2] == 48 || req->data[2] == 49));
    answerAll(host, port, 30);
    for (int i = 0; i < 4; i++) CHECK(w[i].ready());
    step(host, port, 10);
    const mcan::Frame* f = port.last(0x218);
    CHECK(f && f->dlc == 8);
    CHECK(f && slot(*f, 0) == 95); // 10 rad/s ＝ 95.5 rpm、既定 1 rpm/LSB
    CHECK(f && slot(*f, 3) == 382);

    // 刻みを変えると、書けたと確かめるまで止める値
    w[0].configSpeedResolution(0.01f);
    CHECK(!w[0].ready());
    port.tx.clear();
    step(host, port, 10);
    CHECK(slot(*port.last(0x218), 0) == 0);
    req = port.last(0x400);
    CHECK(req && req->data[2] == 49);
    CHECK(std::fabs(mas_can_value_f32(&req->data[3]) - 0.0954930f) < 1e-5f);
    respond(port, *req, MAS_CAN_RES_OK);
    step(host, port, 10);
    CHECK(w[0].ready());
    CHECK(slot(*port.last(0x218), 0) == 1000); // 10 rad/s ÷ 0.01
    CHECK(std::fabs(w[0].speedRange() - 327.67f) < 0.01f);
}

static void bringUp(mcan::Host& host, FakePort& port, uint8_t ch) {
    pushFb(port, ch);
    step(host, port, 1);
    answerAll(host, port);
}

static void test_bundle_and_flush() {
    FakePort port;
    mcan::Host host;
    host.begin(port);
    mcan::Md lift = host.md(4), roller = host.md(5);
    bringUp(host, port, 4);
    bringUp(host, port, 5);

    lift.setCurrent(1.5f);
    port.tx.clear();
    step(host, port, 10);
    const mcan::Frame* f = port.last(0x211);
    CHECK(f && f->dlc == 2 && slot(*f, 4) == 1500); // 後ろの slot は削る（C-41）

    roller.setDuty(0.5f); // 同じ束に別の種類（API-52）
    step(host, port, 10);
    CHECK(roller.commandConflict() && !lift.commandConflict());
    CHECK(host.error() == mcan::Error::CommandConflict);
    CHECK(std::strcmp(mcan::toString(host.error()), "command conflict") == 0);
    CHECK(port.count(0x221) == 0);

    // flush は変わった束だけを今すぐ送る（API-36）
    roller.setCurrent(0.5f);
    step(host, port, 10);
    port.tx.clear();
    lift.setCurrent(1.0f);
    host.flush();
    CHECK(port.count(0x211) == 1 && slot(*port.last(0x211), 4) == 1000);
    host.flush();
    CHECK(port.count(0x211) == 1); // 変わっていなければ送らない

    // duty の既定はブレーキ（API-40）
    mcan::Md d = host.md(8);
    bringUp(host, port, 8);
    d.setDuty(0.0f);
    step(host, port, 10);
    CHECK(slot(*port.last(0x222), 8) == 1);
}

static void test_feedback_and_restart() {
    FakePort port;
    mcan::Host host;
    host.begin(port);
    mcan::Md m = host.md(2);
    m.setSpeed(0);
    pushFb(port, 2, 0x40, 1500, 1000, 0xFFFFF0); // 位置は -16
    step(host, port, 1);
    CHECK(m.online() && m.position() == -16);
    CHECK(std::fabs(m.current() - 1.5f) < 1e-4f);
    CHECK(std::fabs(m.speed() - 1000 * 6.2831853f / 60) < 1e-3f);
    answerAll(host, port);
    pushFb(port, 2, 0x40, 0, 0, 0x000010); // 折り返しをまたいで +32
    step(host, port, 1);
    CHECK(m.position() == 16);
    CHECK(&m.feedback() == &m.feedback() && m.feedback().position == 16);

    step(host, port, 250); // FB が途絶える
    CHECK(!m.online());

    // 再起動: 起動の知らせ → 刻みを先に送り直し、確かめるまで止める値。位置は結び直す
    CHECK(m.ready());
    mcan::Frame boot;
    mas_can_boot_encode(MAS_CAN_KIND_MD, &boot);
    port.rx.push_back(boot);
    port.tx.clear();
    step(host, port, 1);
    CHECK(!m.ready());
    CHECK(port.count(0x400) == 0); // 基板が見えるまでは書きに行かない
    pushFb(port, 2, 0x40, 0, 0, 0); // 基板は 0 から
    step(host, port, 1);
    CHECK(m.position() == 16);
    const mcan::Frame* req = port.last(0x400);
    CHECK(req && (req->data[2] == 48 || req->data[2] == 49));
    answerAll(host, port);
    CHECK(m.ready());
}

static uint32_t g_fault_bits = 0;
static int g_fault_ch = -1;
static int g_setting_error = -1;

static void test_fault() {
    FakePort port;
    mcan::Host host;
    host.begin(port);
    host.onFault([](mcan::Device& dev, uint32_t bits) {
        g_fault_ch = dev.ch();
        g_fault_bits = bits;
    });
    mcan::Md m = host.md(1);
    bringUp(host, port, 1);
    pushFb(port, 1, MAS_CAN_MD_ST_FAULT | 0x40);
    port.tx.clear();
    step(host, port, 1);
    CHECK(m.hasFault());
    const mcan::Frame* req = port.last(0x400);
    CHECK(req && req->data[0] == 1 && req->data[1] == 0x00 && req->data[2] == 5);
    uint32_t detail = 0x101; // 再起動 ＋ 過電流
    respond(port, *req, MAS_CAN_RES_OK, &detail);
    step(host, port, 1);
    CHECK(g_fault_ch == 1 && g_fault_bits == 0x100); // 再起動の bit は知らせない（API-19）
    CHECK(m.faultDetail() == 0x100);
    answerAll(host, port); // 刻みの送り直しと、再起動の bit を消す書き込み
    CHECK(m.ready());
    m.clearFault();
    port.tx.clear();
    step(host, port, 1);
    req = port.last(0x400);
    CHECK(req && req->data[2] == 5 && mas_can_value_u32(&req->data[3]) == 0x100);

    // 設定の失敗は ch ごとに知らせる（API-46）。来なければ 255
    host.onSettingError([](mcan::Device& dev, uint8_t result) {
        g_setting_error = dev.ch() * 1000 + result;
    });
    respond(port, *req, MAS_CAN_RES_OK);
    step(host, port, 1);
    m.configCurrentResolution(0.002f);
    step(host, port, 200);
    CHECK(g_setting_error == 1000 + 255);
    CHECK(!m.ready());
}

static void test_estop_and_sync() {
    FakePort port;
    mcan::Host host;
    host.begin(port);
    mcan::Md m = host.md(0);
    mcan::Servo s = host.servo(0);
    bringUp(host, port, 0);
    m.setCurrent(2.0f);
    s.setPulseUs(1500);
    step(host, port, 20);
    CHECK(slot(*port.last(0x210), 0) == 2000);
    CHECK(port.last(0x230)->data[0] == 100);

    host.estop();
    port.tx.clear();
    step(host, port, 300);
    CHECK(host.estopped());
    CHECK(port.count(0x000) == 3); // 10 Hz（すぐ 1 本 ＋ 100 ms ごと。範囲の端で 3 本）
    CHECK(slot(*port.last(0x210), 0) == 0);
    CHECK(port.last(0x230)->data[0] == MAS_CAN_SERVO_STOP);
    CHECK(port.count(0x002) == 6); // SYNC 20 Hz
    CHECK(port.last(0x002)->dlc == 4);

    host.clearEstop();
    CHECK(!host.estopped());
    CHECK(port.last(0x001) && port.last(0x001)->dlc == 3);
    step(host, port, 20);
    CHECK(slot(*port.last(0x210), 0) == 0); // 解除で指令も止める値に戻る（API-22）
    m.setCurrent(1.0f);
    step(host, port, 10);
    CHECK(slot(*port.last(0x210), 0) == 1000);

    // ほかの送り手の E-stop は見るだけ。解除を受けたら指令を止める値に戻す（API-48）
    mcan::Frame e{};
    e.id = 0;
    port.rx.push_back(e);
    port.tx.clear();
    step(host, port, 300);
    CHECK(host.estopped() && port.count(0x000) == 0);
    mcan::Frame c;
    mas_can_estop_clear_encode(&c);
    port.rx.push_back(c);
    step(host, port, 20);
    CHECK(!host.estopped());
    CHECK(slot(*port.last(0x210), 0) == 0);
}

static void test_controller() {
    FakePort port;
    mcan::Host host;
    host.begin(port);
    mcan::Controller pad = host.controller(0);
    mcan::Frame f{};
    f.id = 0x340;
    f.dlc = 8;
    f.data[0] = 127;                                                 // 左 x
    f.data[1] = (uint8_t)(int8_t)-5;                                 // 左 y
    f.data[2] = 3;                                                   // 右 x（遊びの中）
    f.data[5] = 255;                                                 // R2
    mas_can_put_u16(&f.data[6], (1u << 12) | (1u << 4) | (1u << 15)); // 無線、North、Start
    port.rx.push_back(f);
    pad.configDeadzone(0.05f);
    step(host, port, 1);
    CHECK(pad.online() && pad.connected());
    CHECK(pad.rightX() == 0.0f && pad.r2() == 1.0f);
    CHECK(pad.leftX() > 0.99f && pad.leftY() < 0.0f); // 円で切るので y も残る
    CHECK(pad.held(mcan::Button::North) && pad.pressed(mcan::Button::North));
    CHECK(pad.held(mcan::Button::Start) && !pad.held(mcan::Button::Select));
    step(host, port, 1);
    CHECK(pad.held(mcan::Button::North) && !pad.pressed(mcan::Button::North));

    // 受信機は生きていて、無線だけが切れた
    mas_can_put_u16(&f.data[6], 0);
    port.rx.push_back(f);
    step(host, port, 1);
    CHECK(pad.online() && !pad.connected());

    step(host, port, 250); // 200 ms 来ない
    CHECK(!pad.online() && !pad.connected() && pad.leftX() == 0.0f);
}

static void test_servo() {
    FakePort port;
    mcan::Host host;
    host.begin(port);
    mcan::Servo a = host.servo(1), b = host.servo(2);
    step(host, port, 20);
    const mcan::Frame* f = port.last(0x230);
    CHECK(f && f->dlc == 3 && f->data[0] == MAS_CAN_SERVO_STOP && f->data[1] == 255);

    a.setPulseUs(1500);
    b.setPulseUs(3000); // 2500 に抑える
    step(host, port, 20);
    f = port.last(0x230);
    CHECK(f->data[1] == 100 && f->data[2] == 200);

    a.configCalibration(-1.0f, 1.0f, 2000, 1000); // 向きを逆に
    a.setAngle(0.5f);
    step(host, port, 20);
    CHECK(port.last(0x230)->data[1] == 75); // 1250 µs
    a.release();
    step(host, port, 20);
    CHECK(port.last(0x230)->data[1] == MAS_CAN_SERVO_STOP);

    mcan::Servo far = host.servo(9); // 2 本目の束
    far.setPulseUs(500);
    step(host, port, 20);
    f = port.last(0x231);
    CHECK(f && f->dlc == 2 && f->data[1] == 0);
}

static void test_wait_ready_and_misc() {
    FakePort port;
    mcan::Host host;
    mcan::Md m = host.md(3);
    host.begin(port);
    port.tick = 100;
    CHECK(!host.waitReady(50)); // FB が来ない
    port.tick = 0;
    bringUp(host, port, 3);
    port.tick = 100;
    pushFb(port, 3);
    CHECK(host.waitReady(50));
    port.tick = 0;

    CHECK(host.md(40).ch() == 255); // 範囲外の ch は何もしない取っ手
    CHECK(host.md(3) == m && host.md(3) != host.md(2));

    port.overflows = 7;
    CHECK(host.rxOverflows() == 7);

    // バスの使用率（API-37、API-38）
    host.configBusLoadWarning(0.001f);
    step(host, port, 1100);
    CHECK(host.busLoad() > 0.0f && host.busLoad() < 1.0f);
    CHECK(host.error() == mcan::Error::BusLoadHigh);
    CHECK(std::strcmp(mcan::toString(mcan::Error::None), "") == 0);
}

static int g_errors[4];
static int g_error_count = 0;

static void test_resolution_robustness() {
    // 1. 基板が値を抑えて返したら、書けたとみなし、基板の値で換算して知らせる（M-43）
    FakePort port;
    mcan::Host host;
    host.begin(port);
    g_error_count = 0;
    host.onSettingError([](mcan::Device&, uint8_t result) {
        if (g_error_count < 4) g_errors[g_error_count++] = result;
    });
    mcan::Md m = host.md(0);
    m.configSpeedResolution(0.0001f); // 細かすぎる刻み
    pushFb(port, 0);
    for (int i = 0; i < 20 && !m.ready(); i++) {
        port.tx.clear();
        step(host, port, 1);
        const mcan::Frame* req = port.last(0x400);
        if (!req) continue;
        mas_can_setting_t s;
        mas_can_setting_decode(req, &s);
        if (s.item == 49) {
            mas_can_value_set_f32(s.value, 0.01f); // 基板の下限に抑えた
            s.response = true;
            s.result = MAS_CAN_RES_OK;
            mcan::Frame f;
            mas_can_setting_encode(&s, &f);
            port.rx.push_back(f);
        } else {
            respond(port, *req, MAS_CAN_RES_OK);
        }
        pushFb(port, 0);
    }
    CHECK(m.ready());
    CHECK(g_error_count == 1 && g_errors[0] == MAS_CAN_RES_OK);
    CHECK(std::fabs(m.speedRange() - 0.01f * 6.2831853f / 60 * 32767) < 0.01f);

    // 2. MD が多くても取りこぼさない（列の大きさ 16 を超える）
    FakePort p2;
    mcan::Host h2;
    h2.begin(p2);
    mcan::Md many[12];
    for (int i = 0; i < 12; i++) many[i] = h2.md((uint8_t)i);
    for (int r = 0; r < 60; r++) {
        for (int i = 0; i < 12; i++) pushFb(p2, (uint8_t)i);
        const mcan::Frame* req = p2.last(0x400);
        if (req && req->data[7] != p2.answered_seq) {
            p2.answered_seq = req->data[7];
            respond(p2, *req, MAS_CAN_RES_OK);
        }
        step(h2, p2, 1);
    }
    bool all = true;
    for (int i = 0; i < 12; i++) all &= many[i].ready();
    CHECK(all);

    // 3. 応答が来ずに失敗しても、基板が見えていれば少し待って書き直す
    FakePort p3;
    mcan::Host h3;
    h3.begin(p3);
    mcan::Md w = h3.md(1);
    for (int i = 0; i < 200; i++) { // 応答しない
        pushFb(p3, 1);
        step(h3, p3, 1);
    }
    CHECK(!w.ready());
    for (int i = 0; i < 1100; i++) pushFb(p3, 1), step(h3, p3, 1); // 1 秒待つ
    answerAll(h3, p3, 40);
    CHECK(w.ready());
}

int main() {
    test_resolution_robustness();
    test_ready_and_commands();
    test_bundle_and_flush();
    test_feedback_and_restart();
    test_fault();
    test_estop_and_sync();
    test_controller();
    test_servo();
    test_wait_ready_and_misc();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all host tests passed\n");
    return 0;
}
