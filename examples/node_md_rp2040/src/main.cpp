// 基板側の雛形: RP2040 で 1 ch の MD（duty だけ。電流・速度は測れない）を作る。
//
// プロトコルの処理（指令、途絶、E-stop、起動の猶予、設定、FB）は lib/mas の node が受け持つ。
// loop() で基板がすることは 4 つ:
//   1. 受けたフレームを mas_can_node_on_frame に渡す
//   2. mas_can_node_tick で時間を進める
//   3. mas_can_md_output で出すべき値を受け取り、ハードウェアに出す
//   4. mas_can_node_peek_tx / drop_tx で送るフレームを送る
#include <ACAN2040.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <hardware/pio.h>
#include <hardware/pwm.h>
#include <hardware/sync.h>
#include <pico/unique_id.h>

#include "mas/can/node.h"

#ifndef MD_CH
#define MD_CH 0
#endif
static_assert(MD_CH < MAS_CAN_MD_MAX_CH, "MD_CH: 0..31"); // 範囲の外は、ほかの種類の信号とぶつかる

namespace {

constexpr uint8_t kCanTx = 0, kCanRx = 1, kCanPio = 1;
constexpr uint8_t kPinA = 10, kPinB = 11;               // モータードライバの 2 入力
constexpr uint16_t kPwmTop = (uint16_t)(F_CPU / 20000UL - 1); // 約 20 kHz

mas_can_node_t node;

// ---- CAN: 受信は割り込みで積み、loop() で取り出す ----
constexpr uint8_t kRxq = 32;
mas_can_frame_t rxq[kRxq];
volatile uint8_t rxHead, rxTail;
volatile uint8_t txInFlight; // can2040 に渡して、まだ線に出し終えていない本数

void onCan(struct can2040*, uint32_t notify, struct can2040_msg* msg) {
    if (notify == CAN2040_NOTIFY_TX && txInFlight) txInFlight--;
    if (notify != CAN2040_NOTIFY_RX || (msg->id & CAN2040_ID_EFF)) return;
    if ((uint8_t)(rxHead - rxTail) >= kRxq) return;
    mas_can_frame_t& f = rxq[rxHead % kRxq];
    f.id = msg->id & 0x7FF;
    f.rtr = (msg->id & CAN2040_ID_RTR) != 0;
    f.dlc = msg->dlc > 8 ? 8 : msg->dlc;
    for (int i = 0; i < 8; i++) f.data[i] = msg->data[i];
    rxHead++;
}

ACAN2040 can(kCanPio, kCanTx, kCanRx, 1000000UL, F_CPU, onCan);

bool canSend(const mas_can_frame_t* f) {
    if (!can.ok_to_send()) return false;
    struct can2040_msg m = {};
    m.id = f->id | (f->rtr ? CAN2040_ID_RTR : 0);
    m.dlc = f->dlc;
    for (int i = 0; i < 8; i++) m.data[i] = f->data[i];
    uint32_t irq = save_and_disable_interrupts(); // 送り終えの割り込みと数を食い違わせない
    bool ok = can.send_message(&m);
    if (ok) txInFlight++;
    restore_interrupts(irq);
    return ok;
}

// can2040 は PIO の 4 つの SM を claim せずに使う。ほかのライブラリが空きと見て奪わないよう、先に claim する
void reserveCanPio() {
    PIO pio = kCanPio ? pio1 : pio0;
    for (uint sm = 0; sm < 4; ++sm) {
        if (!pio_sm_is_claimed(pio, sm)) pio_sm_claim(pio, sm);
    }
}

// ---- node に渡す関数: 保存した設定の読み書き、再起動 ----
bool settingsLoad(void*, void* buf, size_t len) {
    EEPROM.begin(sizeof(mas_can_node_saved_t));
    memcpy(buf, EEPROM.getDataPtr(), len);
    return true;
}
// フラッシュに書いている間は割り込みも loop() も止まり、途絶も E-stop も見られない。先に出力を切る。
// 出力は、次の loop() の motorApply が戻す。
bool settingsSave(void*, const void* buf, size_t len) {
    pwm_set_gpio_level(kPinA, 0);
    pwm_set_gpio_level(kPinB, 0);
    memcpy(EEPROM.getDataPtr(), buf, len);
    return EEPROM.commit();
}
// 応答を線に出し終えるのを少し待ってから再起動する
void restart(void*) {
    uint32_t t0 = micros();
    while (txInFlight && (uint32_t)(micros() - t0) < 10000u) {
    }
    rp2040.reboot();
}

uint64_t uniqueId() { // RP2040 はフラッシュの 64 bit の固有 ID
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint64_t v = 0;
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) v = (v << 8) | id.id[i];
    return v;
}

// ---- モーター: 片側に PWM。ブレーキは両方 1、空転は両方 0（ドライバによる） ----
void motorInit() {
    uint slice = pwm_gpio_to_slice_num(kPinA);
    pwm_set_wrap(slice, kPwmTop);
    pwm_set_gpio_level(kPinA, 0);
    pwm_set_gpio_level(kPinB, 0);
    gpio_set_function(kPinA, GPIO_FUNC_PWM);
    gpio_set_function(kPinB, GPIO_FUNC_PWM);
    pwm_set_enabled(slice, true);
}

float motorApply(const mas_can_md_output_t& out) {
    float d = out.mode == MAS_CAN_MD_OUT_DUTY ? out.value : 0.0f;
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    uint16_t on = (uint16_t)((d < 0 ? -d : d) * kPwmTop);
    // 止める、または duty 0 で、ブレーキの指定があればブレーキ。
    // 回している間の brake（PWM を切っている間をブレーキにする）は、ドライバに合わせて書く。雛形は空転のまま
    if ((out.mode != MAS_CAN_MD_OUT_DUTY || d == 0.0f) && out.brake) {
        pwm_set_gpio_level(kPinA, kPwmTop + 1);
        pwm_set_gpio_level(kPinB, kPwmTop + 1);
    } else {
        pwm_set_gpio_level(kPinA, d > 0 ? on : 0);
        pwm_set_gpio_level(kPinB, d < 0 ? on : 0);
    }
    return d;
}

} // namespace

void setup() {
    motorInit();

    mas_can_node_config_t cfg = {};
    cfg.uid = uniqueId();
    cfg.board_type = MAS_CAN_BOARD_TYPE(MAS_CAN_BOARD_RESERVED, 1); // 実際の基板は core/board.h に番号を足す
    cfg.fw_version = 0x00000100u;       // 0.1.0
    cfg.md_first_ch = MD_CH;
    cfg.md_count = 1;
    cfg.md_has_current = false; // 測れない量は false（FB の bit で知らせる）
    cfg.md_has_speed = false;
    cfg.md_has_coast = true;
    cfg.md_fb_rate_max_hz = 200;
    cfg.load = settingsLoad;
    cfg.save = settingsSave;
    cfg.restart = restart;
    mas_can_node_init(&node, &cfg, micros());

    can.begin();
    reserveCanPio();
    mas_can_node_ready(&node, micros()); // 起動の知らせ。ここから 200 ms は指令に従わない
}

void loop() {
    uint32_t now = micros();
    while (rxTail != rxHead) mas_can_node_on_frame(&node, &rxq[rxTail++ % kRxq], now); // 1
    mas_can_node_tick(&node, now);                                                    // 2

    mas_can_md_measure_t m = {}; // 3
    m.duty = motorApply(mas_can_md_output(&node, 0));
    mas_can_md_set_measure(&node, 0, &m);

    const mas_can_frame_t* f; // 4
    while ((f = mas_can_node_peek_tx(&node)) && canSend(f)) mas_can_node_drop_tx(&node);
}
