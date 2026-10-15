// CAN-Servo 基板（RP2040-Zero）のプロトコル v1 のファーム。
//   サーボ 8 ch（種類の番号 1、指令は 0x230〜）: J2〜J9
//   MD 4 ch（duty だけ。電流・速度は測れない）: J1、J10、J11、J12（AM2837）
// MD の処理（指令、FB、設定、途絶、E-stop、起動の猶予）は lib/mas の node が受け持つ。
// サーボはここで扱う（設定の項目 32〜34 は未実装。API-59 で主制御もまだ書かない）。
#include <ACAN2040.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <Servo.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>
#include <hardware/pwm.h>
#include <pico/unique_id.h>

#include "mas/can/core/setting.h"
#include "mas/can/node.h"

#ifndef SERVO_FIRST_CH
#define SERVO_FIRST_CH 0
#endif
#ifndef MD_FIRST_CH
#define MD_FIRST_CH 0
#endif
static_assert(SERVO_FIRST_CH % 8 == 0 && SERVO_FIRST_CH < 64, "SERVO_FIRST_CH: 0, 8, ..., 56");
static_assert(MD_FIRST_CH % 4 == 0 && MD_FIRST_CH < 32, "MD_FIRST_CH: 0, 4, ..., 28");

namespace {

constexpr uint8_t kCanTxPin = 0;
constexpr uint8_t kCanRxPin = 1;
constexpr uint8_t kCanPio = 1;  // PIO0 は Servo ライブラリに残す
constexpr uint8_t kLedPin = 16;

constexpr uint8_t kServoPins[8] = {29, 28, 27, 26, 15, 14, 13, 12};
constexpr uint8_t kMotorPins[4][2] = {{10, 11}, {8, 9}, {6, 7}, {4, 5}};
constexpr uint16_t kPwmTop = (uint16_t)(F_CPU / 20000UL - 1);  // 約 20 kHz

constexpr uint32_t kTimeoutUs = 100000;
constexpr uint32_t kGraceUs = 200000; // 起動の猶予（C-89）
constexpr uint8_t kServoStop = 255;

mas_can_node_t node;
Servo servos[8];
bool servoAttached[8];
uint8_t servoValue[8] = {kServoStop, kServoStop, kServoStop, kServoStop,
                         kServoStop, kServoStop, kServoStop, kServoStop};
uint32_t servoLastUs[8];
bool servoSeen[8];
float appliedDuty[4];
bool servoBootPending;

// 受信は割り込みで積み、loop() で取り出す。
constexpr uint8_t kRxq = 32;
mas_can_frame_t rxq[kRxq];
volatile uint8_t rxHead, rxTail;

void forceOutputsLow() {
    for (uint8_t pin : kServoPins) {
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, false);
    }
    for (const auto &pair : kMotorPins) {
        for (uint8_t pin : pair) {
            gpio_set_function(pin, GPIO_FUNC_SIO);
            gpio_set_dir(pin, GPIO_OUT);
            gpio_put(pin, false);
        }
    }
}

void canCallback(struct can2040 *, uint32_t notify, struct can2040_msg *msg) {
    if (notify != CAN2040_NOTIFY_RX || !msg || (msg->id & CAN2040_ID_EFF)) return;
    uint16_t id = msg->id & 0x7FF;
    // E-stop は割り込みの中ですぐ出力を切る（loop() を待たない）
    if (id == MAS_CAN_ID_ESTOP) forceOutputsLow();
    if ((uint8_t)(rxHead - rxTail) >= kRxq) return;
    mas_can_frame_t &f = rxq[rxHead % kRxq];
    f.id = id;
    f.rtr = (msg->id & CAN2040_ID_RTR) != 0;
    f.dlc = msg->dlc > 8 ? 8 : msg->dlc;
    for (int i = 0; i < 8; i++) f.data[i] = msg->data[i];
    rxHead++;
}

ACAN2040 can(kCanPio, kCanTxPin, kCanRxPin, 1000000UL, F_CPU, canCallback);

bool canSend(const mas_can_frame_t *f) {
    if (!can.ok_to_send()) return false;
    struct can2040_msg m = {};
    m.id = f->id;
    if (f->rtr) m.id |= CAN2040_ID_RTR;
    m.dlc = f->dlc;
    for (int i = 0; i < 8; i++) m.data[i] = f->data[i];
    return can.send_message(&m);
}

void reserveCanPio() {
    // can2040 は PIO1 の 4 つの SM を claim せずに使う。Servo ライブラリが PIO1 を空きと
    // 見て奪わないように、先に claim しておく。
    for (uint sm = 0; sm < 4; ++sm) {
        if (!pio_sm_is_claimed(pio1, sm)) pio_sm_claim(pio1, sm);
    }
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t prog[] = {
        (uint16_t)(pio_encode_out(pio_x, 1) | pio_encode_sideset(1, 0) | pio_encode_delay(2)),
        (uint16_t)(pio_encode_jmp_not_x(3) | pio_encode_sideset(1, 1) | pio_encode_delay(1)),
        (uint16_t)(pio_encode_jmp(0) | pio_encode_sideset(1, 1) | pio_encode_delay(4)),
        (uint16_t)(pio_encode_nop() | pio_encode_sideset(1, 0) | pio_encode_delay(4)),
    };
    const pio_program_t program = {prog, 4, -1, 0};
    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &program);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 3);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, kLedPin);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 8000000.0f);
    pio_gpio_init(pio, kLedPin);
    pio_sm_set_consecutive_pindirs(pio, sm, kLedPin, 1, true);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    pio_sm_put_blocking(pio, sm, grb << 8);
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) tight_loop_contents();
    delayMicroseconds(100);
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_unclaim(pio, sm);
    pio_remove_program(pio, &program, offset);
}

// ---- 設定の保存（node の load/save）。RP2040 のフラッシュの末尾を EEPROM として使う ----

bool settingsLoad(void *, void *buf, size_t len) {
    EEPROM.begin(sizeof(mas_can_node_saved_t));
    memcpy(buf, EEPROM.getDataPtr(), len);
    return true;
}

bool settingsSave(void *, const void *buf, size_t len) {
    memcpy(EEPROM.getDataPtr(), buf, len);
    return EEPROM.commit();
}

void restart(void *) { rp2040.reboot(); }

uint64_t uniqueId() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint64_t v = 0;
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) v = (v << 8) | id.id[i];
    return v;
}

// ---- サーボ ----

void servoReceive(const mas_can_frame_t &f, uint32_t now) {
    if (f.rtr || f.id != mas_can_servo_cmd_id(SERVO_FIRST_CH) || node.estop) return;
    if (!node.ready || (uint32_t)(now - node.ready_us) < kGraceUs) return; // C-89
    for (uint8_t i = 0; i < 8; i++) {
        if (i >= f.dlc) break;  // DLC の外の slot は受けていない扱い（D9）
        uint8_t v = f.data[i];
        servoValue[i] = v > 200 ? kServoStop : v;  // 201〜254 は予約。止める扱い
        servoLastUs[i] = now;
        servoSeen[i] = true;
    }
}

void servoStop(uint8_t i) {
    if (servoAttached[i]) {
        servos[i].detach();
        servoAttached[i] = false;
    }
    pinMode(kServoPins[i], OUTPUT);
    digitalWrite(kServoPins[i], LOW);
}

void servoUpdate(uint32_t now, bool estopEdge) {
    for (uint8_t i = 0; i < 8; i++) {
        bool timedOut = !servoSeen[i] || (uint32_t)(now - servoLastUs[i]) >= kTimeoutUs;
        if (estopEdge || node.estop || timedOut || servoValue[i] == kServoStop) {
            if (estopEdge || node.estop) servoSeen[i] = false;
            servoStop(i);
            continue;
        }
        if (!servoAttached[i]) {
            servoAttached[i] = servos[i].attach(kServoPins[i], 500, 2500) >= 0;
            if (!servoAttached[i]) continue;
        }
        servos[i].writeMicroseconds(500 + servoValue[i] * 10);
    }
}

// ---- MD（AM2837。片側に PWM、オフの間は両方 0 ＝ 空転） ----

void motorPwmInit() {
    for (const auto &pair : kMotorPins) {
        uint slice = pwm_gpio_to_slice_num(pair[0]);
        pwm_set_enabled(slice, false);
        pwm_set_clkdiv(slice, 1.0f);
        pwm_set_wrap(slice, kPwmTop);
        pwm_set_gpio_level(pair[0], 0);
        pwm_set_gpio_level(pair[1], 0);
        gpio_set_function(pair[0], GPIO_FUNC_PWM);
        gpio_set_function(pair[1], GPIO_FUNC_PWM);
        pwm_set_enabled(slice, true);
    }
}

// ブレーキ（AM2837 の両方 1）はデータシートで確かめていないので使わず、空転に丸める
// （E-stop のブレーキ M-51 も。出せない基板は空転に丸める）。
void motorUpdate(uint8_t i) {
    mas_can_md_output_t out = mas_can_md_output(&node, i);
    float duty = out.mode == MAS_CAN_MD_OUT_DUTY ? out.value : 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    if (duty < -1.0f) duty = -1.0f;
    const mas_can_md_settings_t *set = mas_can_md_settings(&node, i);
    if (set && set->reverse) duty = -duty;
    uint16_t level = (uint16_t)((duty < 0 ? -duty : duty) * kPwmTop);
    uint8_t a = kMotorPins[i][0], b = kMotorPins[i][1];
    gpio_set_function(a, GPIO_FUNC_PWM);
    gpio_set_function(b, GPIO_FUNC_PWM);
    pwm_set_gpio_level(a, 0);
    pwm_set_gpio_level(b, 0);
    if (duty > 0) pwm_set_gpio_level(a, level);
    if (duty < 0) pwm_set_gpio_level(b, level);
    appliedDuty[i] = set && set->reverse ? -duty : duty;

    mas_can_md_measure_t m = {};
    m.duty = appliedDuty[i];
    mas_can_md_set_measure(&node, i, &m);
}

}  // namespace

void setup() {
    forceOutputsLow();
    for (uint8_t i = 0; i < 8; i++) servoStop(i);

    mas_can_node_config_t cfg = {};
    cfg.uid = uniqueId();
    cfg.board_type = 0x00020001u;       // CAN-Servo（0x0002）、版 1（C-63）
    cfg.fw_version = 0x00000100u;       // 0.1.0
    cfg.protocol_version = 0x01000001u; // 1.0.0-alpha.1（C-71〜74）
    cfg.md_first_ch = MD_FIRST_CH;
    cfg.md_count = 4;
    cfg.md_has_current = false;
    cfg.md_has_speed = false;
    cfg.md_has_coast = true;
    cfg.md_current_limit_max_ma = 0;
    cfg.md_current_limit_default_ma = 0;
    cfg.md_fb_rate_max_hz = 200;
    cfg.load = settingsLoad;
    cfg.save = settingsSave;
    cfg.restart = restart;
    mas_can_node_init(&node, &cfg, micros());

    can.begin();
    reserveCanPio();
    motorPwmInit();
    setLed(0, 12, 0);
    mas_can_node_ready(&node, micros());
    servoBootPending = true; // サーボの起動の知らせ（C-36）。MD の分は node が送る
}

void loop() {
    static bool lastEstop = false;
    static uint8_t led = 0;  // 0 緑（待機）、1 青（指令を受けている）、2 赤（E-stop）

    uint32_t now = micros();
    while (rxTail != rxHead) {
        const mas_can_frame_t &f = rxq[rxTail % kRxq];
        mas_can_node_on_frame(&node, &f, now);
        servoReceive(f, now);
        rxTail++;
    }
    mas_can_node_tick(&node, now);

    bool estopEdge = node.estop && !lastEstop;
    lastEstop = node.estop;
    servoUpdate(now, estopEdge);
    if (estopEdge) motorPwmInit();  // 割り込みで SIO に切り替えたピンを PWM に戻す
    for (uint8_t i = 0; i < 4; i++) motorUpdate(i);

    const mas_can_frame_t *f;
    while ((f = mas_can_node_peek_tx(&node)) && canSend(f)) mas_can_node_drop_tx(&node);
    if (servoBootPending && !mas_can_node_peek_tx(&node)) {
        mas_can_frame_t boot;
        mas_can_boot_encode(MAS_CAN_KIND_SERVO, &boot);
        if (canSend(&boot)) servoBootPending = false;
    }

    bool active = false;
    for (uint8_t i = 0; i < 8; i++) active |= servoAttached[i];
    for (uint8_t i = 0; i < 4; i++) active |= appliedDuty[i] != 0;
    uint8_t next = node.estop ? 2 : (active ? 1 : 0);
    if (next != led) {
        led = next;
        if (led == 0) setLed(0, 12, 0);
        if (led == 1) setLed(0, 0, 16);
        if (led == 2) setLed(16, 0, 0);
    }
}
