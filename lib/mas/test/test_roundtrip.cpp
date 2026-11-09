// 往復のテスト: Host が作ったフレームを本物の node に食わせ、node の FB と応答を Host に返す。
// 仮想の線で 1 ms ずつ進める。モーターは指令どおりに動く理想のもの。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "mas/can.h"
#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"
#include "mas/can/node.h"

namespace mcan = mas::can;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint32_t g_now = 5000000;
static const float kRpmPerRadps = 60.0f / 6.2831853f;

struct Board {
    mas_can_node_t node;
    mas_can_node_config_t cfg;
    unsigned char flash[sizeof(mas_can_node_saved_t)];
    bool blank = true;
    mas_can_md_output_t out[MAS_CAN_NODE_MD_MAX];
};

static bool flashLoad(void* ctx, void* buf, size_t len) {
    Board* b = (Board*)ctx;
    if (b->blank)
        memset(buf, 0xFF, len);
    else
        memcpy(buf, b->flash, len);
    return true;
}
static bool flashSave(void* ctx, const void* buf, size_t len) {
    Board* b = (Board*)ctx;
    memcpy(b->flash, buf, len);
    b->blank = false;
    return true;
}

struct Bus;
struct BusPort : mcan::Port {
    Bus* bus = nullptr;
    std::deque<mcan::Frame> rx;
    bool send(const mcan::Frame& f) override;
    bool receive(mcan::Frame& f) override {
        if (rx.empty()) return false;
        f = rx.front();
        rx.pop_front();
        return true;
    }
    uint32_t micros() override { return g_now; }
    uint32_t rxOverflows() override { return 0; }
};

struct Bus {
    mcan::Host host;
    BusPort port;
    std::vector<Board*> boards;
    uint16_t drop_id = 0xFFFF; // この ID は Host に届けない（取りこぼし）
    Bus() { port.bus = this; }

    void step(int ms) {
        for (int i = 0; i < ms; i++) {
            g_now += 1000;
            for (Board* b : boards) {
                mas_can_node_tick(&b->node, g_now);
                for (uint8_t l = 0; l < b->node.cfg.md_count; l++) {
                    mas_can_md_output_t o = mas_can_md_output(&b->node, l);
                    b->out[l] = o;
                    mas_can_md_measure_t m = {};
                    if (o.mode == MAS_CAN_MD_OUT_SPEED) m.speed_rpm = o.value;
                    if (o.mode == MAS_CAN_MD_OUT_CURRENT) m.current_ma = o.value;
                    if (o.mode == MAS_CAN_MD_OUT_DUTY) m.duty = o.value;
                    mas_can_md_set_measure(&b->node, l, &m);
                }
                const mas_can_frame_t* f;
                while ((f = mas_can_node_peek_tx(&b->node))) {
                    mcan::Frame c = *f;
                    mas_can_node_drop_tx(&b->node);
                    for (Board* o : boards) // CAN は全員に届く
                        if (o != b) mas_can_node_on_frame(&o->node, &c, g_now);
                    if (c.id != drop_id) port.rx.push_back(c);
                }
            }
            host.update();
        }
    }
};

bool BusPort::send(const mcan::Frame& f) {
    for (Board* b : bus->boards) mas_can_node_on_frame(&b->node, &f, g_now);
    return true;
}

static void boot(Board& b, uint8_t first_ch, uint8_t count) {
    memset(&b.cfg, 0, sizeof(b.cfg));
    b.cfg.uid = 0xABCD0000u + first_ch;
    b.cfg.board_type = MAS_CAN_BOARD_TYPE(MAS_CAN_BOARD_MD_2CH, 1);
    b.cfg.md_first_ch = first_ch;
    b.cfg.md_count = count;
    b.cfg.md_has_current = true;
    b.cfg.md_has_speed = true;
    b.cfg.md_has_coast = true;
    b.cfg.md_current_limit_max_ma = 20000;
    b.cfg.md_current_limit_default_ma = 10000;
    b.cfg.md_fb_rate_max_hz = 1000;
    b.cfg.md_encoder_cpr_default = 4096;
    b.cfg.ctx = &b;
    b.cfg.load = flashLoad;
    b.cfg.save = flashSave;
    mas_can_node_init(&b.node, &b.cfg, g_now);
    mas_can_node_ready(&b.node, g_now);
}

// 基板の出力の速度の最大 [rpm] を ms の間で見る
static float maxSpeed(Bus& bus, Board& b, int ms) {
    float v = 0;
    for (int i = 0; i < ms; i++) {
        bus.step(1);
        if (b.out[0].mode == MAS_CAN_MD_OUT_SPEED) v = std::fmax(v, std::fabs(b.out[0].value));
    }
    return v;
}

static void test_basic() {
    Bus bus;
    Board b;
    bus.boards.push_back(&b);
    mcan::Md w = bus.host.md(0);
    bus.host.begin(bus.port);
    boot(b, 0, 4);
    w.configSpeedResolution(0.01f);
    w.setSpeed(5.0f);
    bus.step(400);
    CHECK(w.ready());
    CHECK(b.out[0].mode == MAS_CAN_MD_OUT_SPEED &&
          std::fabs(b.out[0].value - 5.0f * kRpmPerRadps) < 0.1f);
    CHECK(std::fabs(w.speed() - 5.0f) < 0.02f);
    CHECK(!b.node.fault); // 再起動の bit は Host が消した（API-19）
}

// 基板の再起動。起動の知らせがあってもなくても、桁違いの速度を出さない（C-69、C-94）
static void test_restart(bool lose_boot_notice) {
    Bus bus;
    Board b;
    bus.boards.push_back(&b);
    mcan::Md w = bus.host.md(0);
    bus.host.begin(bus.port);
    boot(b, 0, 1);
    w.configSpeedResolution(0.01f);
    w.setSpeed(5.0f);
    bus.step(400);
    CHECK(w.ready());
    if (lose_boot_notice) bus.drop_id = 0x010;
    boot(b, 0, 1);
    CHECK(maxSpeed(bus, b, 600) < 5.0f * kRpmPerRadps * 1.05f);
    CHECK(w.ready());
}

// 前から異常が立っている基板の再起動。起動の知らせを取りこぼしても、
// 異常の詳細の読み直しで見つける（C-69、API-66）。新しく立った bit も知らせる
static uint32_t g_fault_bits = 0;
static void test_restart_with_standing_fault() {
    Bus bus;
    Board b;
    bus.boards.push_back(&b);
    mcan::Md w = bus.host.md(0);
    bus.host.onFault([](mcan::Device&, uint32_t bits) { g_fault_bits |= bits; });
    bus.host.begin(bus.port);
    boot(b, 0, 1);
    w.configSpeedResolution(0.01f);
    w.setSpeed(5.0f);
    bus.step(400);
    mas_can_node_raise_fault(&b.node, 0x400); // アプリは消さない
    bus.step(100);
    CHECK(g_fault_bits == 0x400);
    mas_can_node_raise_fault(&b.node, 0x100); // 異常が立ったまま、別の bit が加わる
    bus.step(1100);
    CHECK(g_fault_bits == 0x500);

    bus.drop_id = 0x010;
    boot(b, 0, 1);
    mas_can_node_raise_fault(&b.node, 0x500); // 起動後も同じ異常が続く
    // 読み直しまでの間は、猶予（C-89）と止める値で守られる
    CHECK(maxSpeed(bus, b, 2000) < 5.0f * kRpmPerRadps * 1.05f);
    CHECK(w.ready() && mas_can_md_settings(&b.node, 0)->speed_lsb_rpm != 1.0f);
}

// api_tour の配置: lift（ch 4、電流）と roller（ch 8、duty）は別の束なので両方動く
static void test_api_tour_layout() {
    Bus bus;
    Board b4, b8;
    bus.boards.push_back(&b4);
    bus.boards.push_back(&b8);
    mcan::Md lift = bus.host.md(4), roller = bus.host.md(8);
    bus.host.begin(bus.port);
    boot(b4, 4, 1);
    boot(b8, 8, 1);
    bus.step(400);
    lift.setCurrent(1.0f);
    roller.setDuty(0.6f);
    bus.step(200);
    CHECK(b4.out[0].mode == MAS_CAN_MD_OUT_CURRENT && b8.out[0].mode == MAS_CAN_MD_OUT_DUTY);
    CHECK(!roller.commandConflict() && bus.host.error() == mcan::Error::None);
}

int main() {
    test_basic();
    test_restart(false);
    test_restart(true);
    test_restart_with_standing_fault();
    test_api_tour_layout();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all roundtrip tests passed\n");
    return 0;
}
