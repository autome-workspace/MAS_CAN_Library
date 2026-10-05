// 主制御のライブラリ（Bus、Md、Controller）のテスト。偽のポートで PC で動かす。
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

#include "mas/can.h"
#include "mas/can/core/md.h"
#include "mas/can/core/setting.h"

namespace mcan = mas::can;

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
    std::deque<mcan::Frame> rx;
    std::vector<mcan::Frame> tx;
    bool send(const mcan::Frame &f) override {
        tx.push_back(f);
        return true;
    }
    bool receive(mcan::Frame &f) override {
        if (rx.empty()) return false;
        f = rx.front();
        rx.pop_front();
        return true;
    }
    uint32_t micros() override { return now; }

    const mcan::Frame *last(uint16_t id) const {
        for (auto it = tx.rbegin(); it != tx.rend(); ++it)
            if (it->id == id) return &*it;
        return nullptr;
    }
    int count(uint16_t id) const {
        int n = 0;
        for (auto &f : tx) n += f.id == id;
        return n;
    }
};

static int16_t slot(const mcan::Frame &f, int ch) {
    int16_t v = 0;
    mas_can_md_cmd_get(&f, (uint8_t)ch, &v);
    return v;
}

static void step(mcan::Bus &bus, FakePort &port, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        port.now += 1000;
        bus.update();
    }
}

static void respond(FakePort &port, const mcan::Frame &req, uint8_t result, uint32_t value) {
    mas_can_setting_t s;
    mas_can_setting_decode(&req, &s);
    s.response = true;
    s.result = result;
    mas_can_value_set_u32(s.value, value);
    mcan::Frame f;
    mas_can_setting_encode(&s, &f);
    port.rx.push_back(f);
}

static void pushFb(FakePort &port, uint8_t ch, uint8_t status, int16_t cur, int16_t spd,
                   uint32_t pos) {
    mas_can_md_fb_t fb = {};
    fb.status = status;
    fb.current = cur;
    fb.speed = spd;
    fb.position = pos;
    mcan::Frame f;
    mas_can_md_fb_encode(&fb, ch, &f);
    port.rx.push_back(f);
}

static void test_commands() {
    FakePort port;
    mcan::Bus bus;
    bus.begin(port);
    mcan::Md w[4] = {bus.md(0), bus.md(1), bus.md(2), bus.md(3)};
    for (int i = 0; i < 4; i++) w[i].setSpeed(10.0f * (i + 1));  // rad/s
    step(bus, port, 1);
    const mcan::Frame *f = port.last(0x218);
    CHECK(f && f->dlc == 8);
    CHECK(f && slot(*f, 0) == 95);  // 10 rad/s ＝ 95.5 rpm、既定 1 rpm/LSB
    CHECK(f && slot(*f, 3) == 382);

    // 分解能を変えると、書けたと確かめるまで止める値（A4）
    w[0].setSpeedResolution(0.01f);
    CHECK(!w[0].ready());
    port.tx.clear();
    step(bus, port, 10);
    f = port.last(0x218);
    CHECK(f && slot(*f, 0) == 0);
    const mcan::Frame *req = port.last(0x400);
    CHECK(req && req->data[2] == 49 && req->data[1] == 0x40);
    float lsb = mas_can_value_f32(&req->data[3]);
    CHECK(std::fabs(lsb - 0.0954930f) < 1e-5f);
    respond(port, *req, MAS_CAN_RES_OK, 0);
    port.tx.clear();
    step(bus, port, 10);
    CHECK(w[0].ready());
    f = port.last(0x218);
    CHECK(f && slot(*f, 0) == 1000);  // 10 rad/s ÷ 0.01

    // DLC を縮める、同じ組に別の種類（衝突）
    mcan::Md lift = bus.md(4), roller = bus.md(5);
    lift.setCurrent(1.5f);
    port.tx.clear();
    step(bus, port, 10);
    f = port.last(0x211);
    CHECK(f && f->dlc == 2 && slot(*f, 4) == 1500);
    roller.setDuty(0.5f, true);
    step(bus, port, 10);
    CHECK(roller.commandConflict() && !lift.commandConflict());
    CHECK(port.count(0x221) == 0);
}

static void test_feedback_and_restart() {
    FakePort port;
    mcan::Bus bus;
    bus.begin(port);
    mcan::Md m = bus.md(2);
    m.setSpeed(0);
    pushFb(port, 2, 0x40, 1500, 1000, 0xFFFFF0);  // 位置は -16
    step(bus, port, 1);
    CHECK(m.online() && m.position() == -16);
    CHECK(std::fabs(m.current() - 1.5f) < 1e-4f);
    CHECK(std::fabs(m.speed() - 1000 * 6.2831853f / 60) < 1e-3f);
    pushFb(port, 2, 0x40, 0, 0, 0x000010);  // 折り返しをまたいで +32
    step(bus, port, 1);
    CHECK(m.position() == 16);
    const auto &fb = m.feedback();
    CHECK(&fb == &m.feedback() && fb.position == 16);

    step(bus, port, 250);  // FB が途絶える
    CHECK(!m.online());

    // 再起動: 起動の知らせ → 分解能を送り直し、確かめるまで止める値。位置は結び直す
    m.setSpeedResolution(0.01f);
    step(bus, port, 10);
    respond(port, *port.last(0x400), MAS_CAN_RES_OK, 0);
    step(bus, port, 10);
    CHECK(m.ready());
    mcan::Frame boot;
    mas_can_boot_encode(MAS_CAN_KIND_MD, &boot);
    port.rx.push_back(boot);
    step(bus, port, 1);
    CHECK(!m.ready());
    pushFb(port, 2, 0x40, 0, 0, 0);  // 基板は 0 から
    step(bus, port, 10);
    CHECK(m.position() == 16);
    respond(port, *port.last(0x400), MAS_CAN_RES_OK, 0);
    step(bus, port, 10);
    CHECK(m.ready());
}

static uint32_t g_fault_bits = 0;
static int g_fault_ch = -1;

static void test_fault() {
    FakePort port;
    mcan::Bus bus;
    bus.begin(port);
    bus.onFault([](mcan::Md &md, uint32_t bits) {
        g_fault_ch = md.ch();
        g_fault_bits = bits;
    });
    mcan::Md m = bus.md(1);
    pushFb(port, 1, MAS_CAN_MD_ST_FAULT, 0, 0, 0);
    step(bus, port, 1);
    CHECK(m.hasFault());
    const mcan::Frame *req = port.last(0x400);
    CHECK(req && req->data[0] == 1 && req->data[1] == 0x00 && req->data[2] == 5);
    respond(port, *req, MAS_CAN_RES_OK, 0x101);  // 再起動 ＋ 過電流
    port.tx.clear();
    step(bus, port, 1);
    CHECK(g_fault_ch == 1 && g_fault_bits == 0x100);  // 再起動の bit は知らせない（A7）
    CHECK(m.faultDetail() == 0x100);
    req = port.last(0x400);  // 再起動の bit を消す書き込み
    CHECK(req && req->data[1] == 0x40 && req->data[2] == 5 &&
          mas_can_value_u32(&req->data[3]) == 1);
    respond(port, *req, MAS_CAN_RES_OK, 0);
    step(bus, port, 1);
    m.clearFault();  // アプリが消す
    port.tx.clear();
    step(bus, port, 1);
    req = port.last(0x400);
    CHECK(req && mas_can_value_u32(&req->data[3]) == 0x100);
}

static void test_estop_and_sync() {
    FakePort port;
    mcan::Bus bus;
    bus.begin(port);
    mcan::Md m = bus.md(0);
    m.setCurrent(2.0f);
    step(bus, port, 1);
    CHECK(slot(*port.last(0x210), 0) == 2000);

    bus.estop();
    port.tx.clear();
    step(bus, port, 300);
    CHECK(bus.estopped());
    CHECK(port.count(0x000) == 3);           // 10 Hz で送り続ける（A8）
    CHECK(slot(*port.last(0x210), 0) == 0);  // その間は止める値
    CHECK(port.count(0x002) == 6);           // SYNC 20 Hz
    CHECK(port.last(0x002)->dlc == 4);

    bus.clearEstop();
    CHECK(!bus.estopped());
    CHECK(port.last(0x001) && port.last(0x001)->dlc == 3);
    step(bus, port, 20);
    CHECK(slot(*port.last(0x210), 0) == 0);  // 解除のとき指令も止める値に戻した（D21）
    m.setCurrent(1.0f);
    step(bus, port, 10);
    CHECK(slot(*port.last(0x210), 0) == 1000);

    // ほかの送り手の E-stop は見るだけ（送り直さない）
    mcan::Frame e{};
    e.id = 0;
    port.rx.push_back(e);
    port.tx.clear();
    step(bus, port, 300);
    CHECK(bus.estopped() && port.count(0x000) == 0);
    mcan::Frame c;
    mas_can_estop_clear_encode(&c);
    port.rx.push_back(c);
    step(bus, port, 1);
    CHECK(!bus.estopped());
}

static void test_controller() {
    FakePort port;
    mcan::Bus bus;
    bus.begin(port);
    mcan::Controller pad = bus.controller(0);
    mcan::Frame f{};
    f.id = 0x340;
    f.dlc = 8;
    f.data[0] = 127;                                      // 左 x
    f.data[1] = (uint8_t)(int8_t)-5;                      // 左 y（遊びの中）
    f.data[5] = 255;                                      // R2
    mas_can_put_u16(&f.data[6], (1u << 12) | (1u << 4));  // つながっている、North
    port.rx.push_back(f);
    pad.setDeadzone(0.05f);
    step(bus, port, 1);
    CHECK(pad.connected() && pad.leftX() == 1.0f && pad.leftY() == 0.0f && pad.r2() == 1.0f);
    CHECK(pad.held(mcan::Button::North) && pad.pressed(mcan::Button::North));
    step(bus, port, 1);
    CHECK(pad.held(mcan::Button::North) && !pad.pressed(mcan::Button::North));
    step(bus, port, 250);  // 200 ms 来ない
    CHECK(!pad.connected() && !pad.held(mcan::Button::North) && pad.leftX() == 0.0f);
}

int main() {
    test_commands();
    test_feedback_and_restart();
    test_fault();
    test_estop_and_sync();
    test_controller();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all bus tests passed\n");
    return 0;
}
