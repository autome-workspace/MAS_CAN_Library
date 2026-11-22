// XDrive の中継機（RP2040）。主バス（MAS CAN v1、1 Mbps）では 4 ch の MD に見え、
// 下のバスで XDrive を 4 台まで動かす。中身は relay.cpp。ここはハードウェアとのつなぎだけ。
//   主バス:   can2040 on PIO1（MAS_TX / MAS_RX）
//   下のバス: can2040 on PIO0（XD_TX / XD_RX）
#include <ACAN2040.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <pico/unique_id.h>

#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"
#include "relay.h"

#ifndef MAS_TX
#define MAS_TX 0
#endif
#ifndef MAS_RX
#define MAS_RX 1
#endif
#ifndef XD_TX
#define XD_TX 3
#endif
#ifndef XD_RX
#define XD_RX 2
#endif
#ifndef MD_FIRST_CH
#define MD_FIRST_CH 0
#endif
#ifndef XD_BAUD
#define XD_BAUD 250000
#endif
// ch ごとの XDrive の node_id（先頭の ch から順に）。使わない ch は 255
#ifndef XD_NODE0
#define XD_NODE0 0
#endif
#ifndef XD_NODE1
#define XD_NODE1 1
#endif
#ifndef XD_NODE2
#define XD_NODE2 2
#endif
#ifndef XD_NODE3
#define XD_NODE3 3
#endif
#ifndef XD_KT
#define XD_KT 0.014768f
#endif
#ifndef XD_VEL_LIMIT
#define XD_VEL_LIMIT 18.0f
#endif
#ifndef XD_CURRENT_LIMIT_MA
#define XD_CURRENT_LIMIT_MA 12000
#endif
#ifndef XD_ACCEL
#define XD_ACCEL 0.0f  // [turn/s²] モータ軸。設定 46 が 0 のときの加速度（0 ＝ 制限なし）
#endif
#ifndef XD_FRICTION
#define XD_FRICTION 0.0f  // [Nm] モータ軸。摩擦の FF（0 ＝ なし）
#endif
#ifndef XD_INERTIA
#define XD_INERTIA 0.0f  // [Nm/(turn/s²)] モータ軸。速度の FF（0 ＝ なし）
#endif
static_assert(MD_FIRST_CH % 4 == 0 && MD_FIRST_CH < 32, "MD_FIRST_CH: 0, 4, ..., 28");

#ifdef DEBUG_SERIAL
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

namespace {

relay::Relay relayCore;

// ---- 主バス。受信は割り込みで積み、loop() で取り出す ----
constexpr uint8_t kMasq = 32;
mas_can_frame_t masq[kMasq];
volatile uint8_t masHead, masTail;

void onMas(struct can2040*, uint32_t notify, struct can2040_msg* msg) {
    if (notify != CAN2040_NOTIFY_RX || (msg->id & CAN2040_ID_EFF)) return;
    if ((uint8_t)(masHead - masTail) >= kMasq) return;
    mas_can_frame_t& f = masq[masHead % kMasq];
    f.id = msg->id & 0x7FF;
    f.rtr = (msg->id & CAN2040_ID_RTR) != 0;
    f.dlc = msg->dlc > 8 ? 8 : msg->dlc;
    for (int i = 0; i < 8; i++) f.data[i] = msg->data[i];
    masHead++;
}

ACAN2040 mas(1, MAS_TX, MAS_RX, 1000000UL, F_CPU, onMas);

bool masSend(const mas_can_frame_t* f) {
    if (!mas.ok_to_send()) return false;
    struct can2040_msg m = {};
    m.id = f->id | (f->rtr ? CAN2040_ID_RTR : 0);
    m.dlc = f->dlc;
    for (int i = 0; i < 8; i++) m.data[i] = f->data[i];
    return mas.send_message(&m);
}

// ---- 下のバス ----
constexpr uint8_t kXdq = 64;
xd::Frame xdq[kXdq];
volatile uint8_t xdHead, xdTail;

void onXd(struct can2040*, uint32_t notify, struct can2040_msg* msg) {
    if (notify != CAN2040_NOTIFY_RX || (msg->id & CAN2040_ID_EFF)) return;
    if ((uint8_t)(xdHead - xdTail) >= kXdq) return;
    xd::Frame& f = xdq[xdHead % kXdq];
    f.id = msg->id & 0x7FF;
    f.rtr = (msg->id & CAN2040_ID_RTR) != 0;
    f.dlc = msg->dlc > 8 ? 8 : msg->dlc;
    for (int i = 0; i < 8; i++) f.data[i] = msg->data[i];
    xdHead++;
}

ACAN2040 xdBus(0, XD_TX, XD_RX, XD_BAUD, F_CPU, onXd);

// can2040 は ACK の来ないフレームを止めずに送り直す。XDrive が全部消える（主電源を切る）と、宛先の無い
// Estop を毎秒 2000 本ほど送り直し続け、戻ってきた XDrive がその中で起きて bus-off になり、電源を
// 入れ直すまで黙る（fw 0.5.1 は自分で戻らない。2026-10-28 に 2 度）。渡したフレームが kXdStuckUs
// 1 本も通らなければ、下のバスを止めて送り待ちを捨てる
constexpr uint32_t kXdStuckUs = 30000;
uint32_t xdHanded, xdBase, xdProgressAt, xdFlushes;

uint32_t xdTxTotal() {
    can2040_stats xs;
    xdBus.get_statistics(&xs);
    return xs.tx_total;
}

bool xdSend(const xd::Frame* f) {
    if (!xdBus.ok_to_send()) return false;
    struct can2040_msg m = {};
    m.id = f->id | (f->rtr ? CAN2040_ID_RTR : 0);
    m.dlc = f->dlc;
    for (int i = 0; i < 8; i++) m.data[i] = f->data[i];
    if (!xdBus.send_message(&m)) return false;
    xdHanded++;
    return true;
}

void xdWatch(uint32_t now) {
    static uint32_t lastTx;
    uint32_t tx = xdTxTotal();
    if (tx != lastTx || xdHanded == xdBase + tx) {  // 通った / 送り待ちが無い
        lastTx = tx;
        xdProgressAt = now;
        return;
    }
    if ((uint32_t)(now - xdProgressAt) < kXdStuckUs) return;
    xdBus.stop();
    xdBus.begin();  // can2040_setup が送り待ちと統計を消す
    xdBase = xdHanded;
    lastTx = 0;
    xdProgressAt = now;
    xdFlushes++;
}

// ---- 設定の保存（フラッシュの末尾を EEPROM として使う） ----
bool settingsLoad(void*, void* buf, size_t len) {
    EEPROM.begin(sizeof(mas_can_node_saved_t));
    memcpy(buf, EEPROM.getDataPtr(), len);
    return true;
}

bool settingsSave(void*, const void* buf, size_t len) {
    memcpy(EEPROM.getDataPtr(), buf, len);
    return EEPROM.commit();
}

void restart(void*) { rp2040.reboot(); }

uint64_t uniqueId() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint64_t v = 0;
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) v = (v << 8) | id.id[i];
    return v;
}

#ifdef DEBUG_SERIAL
void report(uint32_t now) {
    // 読み手が詰まっているときは書かない。USB の書き込みで待たされると中継が止まり、主制御から OFFLINE、
    // 全 ch が heartbeat-lost になる（2026-10-28、シリアルを開き直した瞬間に起きた）。次の周回で書く
    // 1 行（最長 170 文字ほど）が入る空きが無ければ、その行は次の周回に回す（送信バッファは 256 バイト）
    // 読み手がいれば USB が空けてくれるので少し（5 ms まで）待つ。読み手が詰まっていれば待たずに飛ばす
    auto room = [] {
        const uint32_t t0 = micros();
        while (Serial.availableForWrite() < 200) {
            if ((uint32_t)(micros() - t0) >= 5000) return false;
        }
        return true;
    };
    // 状態が変わった瞬間に 1 行（時刻 ms、受信エラーの数も）
    static relay::Link prevLink[relay::kCh];
    for (int i = 0; i < relay::kCh; i++) {
        const relay::Channel& c = relayCore.channel(i);
        if (c.link == prevLink[i]) continue;
        if (!room()) return;
        prevLink[i] = c.link;
        can2040_stats xs;
        xdBus.get_statistics(&xs);
        Serial.printf("! t=%lu ch%d -> %s (%s) state=%u err=%08lx hb_age=%lu ms xd perr=%lu rx=%lu\n",
                      (unsigned long)(now / 1000), MD_FIRST_CH + i, relay::name(c.link),
                      relay::name(c.why), c.state, (unsigned long)c.axis_error,
                      (unsigned long)((now - c.hb_at) / 1000), (unsigned long)xs.parse_error,
                      (unsigned long)xs.rx_total);
    }
    static uint32_t last;
    if ((uint32_t)(now - last) < 1000000 || !room()) return;
    last = now;
    can2040_stats ms, xs;
    mas.get_statistics(&ms);
    xdBus.get_statistics(&xs);
    const mas_can_node_t& n = relayCore.node();
    // RX のピンの今のレベル（線が静かなら 1 ＝ recessive。0 のままならトランシーバか配線）
    Serial.printf("t=%lu estop=%d fault=%08lx mas rx=%lu tx=%lu perr=%lu pin=%d | xd rx=%lu tx=%lu perr=%lu pin=%d drop=%lu flush=%lu\n",
                  (unsigned long)(now / 1000), n.estop, (unsigned long)n.fault,
                  (unsigned long)ms.rx_total, (unsigned long)ms.tx_total,
                  (unsigned long)ms.parse_error, gpio_get(MAS_RX), (unsigned long)xs.rx_total,
                  (unsigned long)xs.tx_total, (unsigned long)xs.parse_error, gpio_get(XD_RX),
                  (unsigned long)relayCore.xdDropped(), (unsigned long)xdFlushes);
    for (int i = 0; i < relay::kCh; i++) {
        const relay::Channel& c = relayCore.channel(i);
        if (!room()) return;
        // printf の %f は使えない（小数は 1000 倍の整数で出す）
        Serial.printf("  ch%d %-9s %-16s state=%u err=%08lx merr=%08lx vel=%ld pos=%ld iq=%ld (x1000)\n",
                      MD_FIRST_CH + i, relay::name(c.link), relay::name(c.why), c.state,
                      (unsigned long)c.axis_error, (unsigned long)c.motor_error,
                      (long)(c.vel * 1000), (long)((c.pos - c.pos0) * 1000), (long)(c.iq * 1000));
    }
}

// シリアルから主バスの代わりをする（主制御が無くても下のバスを試せる）。1 行 1 コマンド:
//   v <ch> <rpm>   速度の指令を 10 ms ごとに入れ続ける    c <ch> <mA>   電流の指令
//   x              入れるのをやめる（100 ms で途絶 → 止まる）
//   e / r          E-stop / 解除        f   異常の詳細を消す（Fault の解除）
//   w <ch> <item> <値>   設定を書く（37 減速比、45、46 は小数。ほかは整数）
mas_can_md_cmd_t injKind;
int injCh = -1;
int16_t injValue;

void injectSetting(uint8_t ch, uint8_t item, const char* text) {
    mas_can_setting_t st = {};
    st.kind = MAS_CAN_KIND_MD;
    st.ch = ch;
    st.op = MAS_CAN_OP_WRITE;
    st.item = item;
    bool f32 = item == 37 || (item >= 39 && item <= 41) || item == 45 || item == 46 || item >= 48;
    if (f32)
        mas_can_value_set_f32(st.value, strtof(text, nullptr));
    else
        mas_can_value_set_u32(st.value, strtoul(text, nullptr, 0));
    mas_can_frame_t f;
    mas_can_setting_encode(&st, &f);
    relayCore.onMas(f, micros());
}

void console(uint32_t now) {
    static char line[48];
    static uint8_t len;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch != '\n' && ch != '\r') {
            if (len < sizeof(line) - 1) line[len++] = ch;
            continue;
        }
        line[len] = 0;
        len = 0;
        int a = 0, b = 0;
        char rest[24] = "";
        mas_can_frame_t f = {};
        switch (line[0]) {
            case 'v':
            case 'c':
                if (sscanf(line + 1, "%d %d", &a, &b) != 2) break;
                injKind = line[0] == 'v' ? MAS_CAN_MD_CMD_SPEED : MAS_CAN_MD_CMD_CURRENT;
                injCh = a;
                injValue = (int16_t)b;
                Serial.printf("> inject %s ch%d %d\n", line[0] == 'v' ? "speed" : "current", a, b);
                break;
            case 'x':
                injCh = -1;
                Serial.printf("> inject off\n");
                break;
            case 'e':
                f.id = MAS_CAN_ID_ESTOP;
                relayCore.onMas(f, now);
                Serial.printf("> estop\n");
                break;
            case 'r':
                mas_can_estop_clear_encode(&f);
                relayCore.onMas(f, now);
                Serial.printf("> estop clear\n");
                break;
            case 'f': {
                char v[16];
                snprintf(v, sizeof(v), "%lu", 0xFFFFFFFFul);
                injectSetting(MD_FIRST_CH, MAS_CAN_ITEM_FAULT_DETAIL, v);
                Serial.printf("> clear fault detail\n");
                break;
            }
            case 'w':
                if (sscanf(line + 1, "%d %d %23s", &a, &b, rest) != 3) break;
                injectSetting((uint8_t)a, (uint8_t)b, rest);
                Serial.printf("> write ch%d item %d = %s\n", a, b, rest);
                break;
            default:
                break;
        }
    }
    static uint32_t last;
    if (injCh >= 0 && (uint32_t)(now - last) >= 10000) {
        last = now;
        mas_can_frame_t f;
        mas_can_md_cmd_init(&f, injKind, (uint8_t)injCh);
        mas_can_md_cmd_set(&f, (uint8_t)injCh, injValue);
        relayCore.onMas(f, now);
    }
}
#endif

}  // namespace

void setup() {
#ifdef DEBUG_SERIAL
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(10);
#endif
    relay::Config cfg = {};
    cfg.node.uid = uniqueId();
    cfg.node.board_type = MAS_CAN_BOARD_TYPE(MAS_CAN_BOARD_RESERVED, 1);  // 番号は core/board.h に取る
    cfg.node.fw_version = 0x00000100u;  // 0.1.0
    cfg.node.md_first_ch = MD_FIRST_CH;
    cfg.node.load = settingsLoad;
    cfg.node.save = settingsSave;
    cfg.node.restart = restart;
    const uint8_t nodes[relay::kCh] = {XD_NODE0, XD_NODE1, XD_NODE2, XD_NODE3};
    for (int i = 0; i < relay::kCh; i++) cfg.xd_node[i] = nodes[i];
    cfg.xd_baud = XD_BAUD;
    cfg.kt = XD_KT;
    cfg.vel_limit = XD_VEL_LIMIT;
    cfg.current_limit_ma = XD_CURRENT_LIMIT_MA;
    cfg.accel = XD_ACCEL;
    cfg.inertia = XD_INERTIA;
    cfg.friction = XD_FRICTION;
    relayCore.begin(cfg, micros());

    xdBus.begin();
    mas.begin();
    DBG("boot md_first=%d xd_baud=%lu nodes=%u,%u,%u,%u period=%lu us\n", MD_FIRST_CH,
        (unsigned long)XD_BAUD, nodes[0], nodes[1], nodes[2], nodes[3],
        (unsigned long)relayCore.periodUs());
    relayCore.ready(micros());
}

void loop() {
    const uint32_t now = micros();
    while (masTail != masHead) relayCore.onMas(masq[masTail++ % kMasq], now);
    while (xdTail != xdHead) relayCore.onXd(xdq[xdTail++ % kXdq], now);
    relayCore.tick(now);

    const mas_can_frame_t* f;
    while ((f = relayCore.peekMas()) && masSend(f)) relayCore.dropMas();
    const xd::Frame* g;
    while ((g = relayCore.peekXd()) && xdSend(g)) relayCore.dropXd();
    xdWatch(now);
#ifdef DEBUG_SERIAL
    console(now);
    report(now);
#endif
}
