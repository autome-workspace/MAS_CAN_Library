// 基板側の API（mas/can/node.h）の使い方の例。2ch-MD（STM32F042、1 ch）を想定。
// HAL の呼び出しは形だけ。まだビルドしない（API を確かめるための下書き）。
#include "main.h"  // CubeMX が生成するもの（hcan、htim2、hadc など）

#include "mas/can/node.h"

static mas_can_node_t node;

// ---- CAN（受信は割り込みでリングバッファに積むだけ） ----
static mas_can_frame_t rx_buf[16];
static volatile uint8_t rx_head, rx_tail;

extern "C" void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
    CAN_RxHeaderTypeDef h;
    mas_can_frame_t &f = rx_buf[rx_head % 16];
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &h, f.data) != HAL_OK || h.IDE != CAN_ID_STD)
        return;
    f.id = h.StdId;
    f.dlc = h.DLC;
    f.rtr = h.RTR == CAN_RTR_REMOTE;
    rx_head++;
}

static bool canSend(const mas_can_frame_t &f) {
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0) return false;
    CAN_TxHeaderTypeDef h = {};
    h.StdId = f.id;
    h.IDE = CAN_ID_STD;
    h.RTR = f.rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    h.DLC = f.dlc;
    uint32_t mailbox;
    return HAL_CAN_AddTxMessage(&hcan, &h, (uint8_t *)f.data, &mailbox) == HAL_OK;
}

// ---- 基板の性質 ----
static uint8_t readDip() {  // 印字 1 ＝ bit 0（PA5）、2 ＝ PB1、3 ＝ PA4。on で 0
    uint8_t k = 0;
    if (!HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_5)) k |= 1;
    if (!HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1)) k |= 2;
    if (!HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4)) k |= 4;
    return k;
}

static uint64_t uniqueId() {  // D22
    const uint32_t *w = (const uint32_t *)UID_BASE;
    return ((uint64_t)(w[2] ^ w[1]) << 32) | w[0];
}

static bool flashLoad(void *, void *buf, size_t len);        // 最後のページから読む
static bool flashSave(void *, const void *buf, size_t len);  // 最後のページに書く
static void restart(void *) { NVIC_SystemReset(); }

static uint32_t nowUs() {
    return HAL_GetTick() * 1000u;
}  // MD は時刻を FB に載せないので 1 ms で足りる

// ---- モーター ----
static void motorApply(const mas_can_md_output_t &out);  // TIM2 の PWM、TIM3 の VREF
static float readCurrentMa();                            // ADC（PWM のオンの区間）

int main() {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_CAN_Init();
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_ADC_Init();

    mas_can_node_config_t cfg = {};
    cfg.uid = uniqueId();
    cfg.board_type = 0x00010001;        // 種類 0x0001、版 1
    cfg.fw_version = 0x00000100;        // 0.1.0
    cfg.protocol_version = 0x00000000;  // 未定（Q14）
    cfg.md_first_ch = readDip();
    cfg.md_count = 1;
    cfg.md_has_current = true;
    cfg.md_has_speed = false;  // エンコーダ無し → 速度・位置の指令は止める（D10）
    cfg.md_has_coast = false;  // 空転を出せない → ブレーキに丸める
    cfg.md_current_limit_max_ma = 10000;
    cfg.md_current_limit_default_ma = 2400;
    cfg.md_fb_rate_max_hz = 1000;
    cfg.load = flashLoad;
    cfg.save = flashSave;
    cfg.restart = restart;
    mas_can_node_init(&node, &cfg, nowUs());

    HAL_CAN_Start(&hcan);
    HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING);
    mas_can_node_ready(&node, nowUs());  // ここで起動の知らせを積む（D30）

    float integral = 0;
    for (;;) {
        uint32_t now = nowUs();

        while (rx_tail != rx_head) {
            mas_can_node_on_frame(&node, &rx_buf[rx_tail % 16], now);
            rx_tail++;
        }
        mas_can_node_tick(&node, now);

        // コアが決めた「出すべきもの」をハードウェアに出す
        mas_can_md_output_t out = mas_can_md_output(&node, 0);
        const mas_can_md_settings_t *set = mas_can_md_settings(&node, 0);
        if (out.reset_ctrl) integral = 0;
        if (out.mode == MAS_CAN_MD_OUT_CURRENT) {
            // 電流の PI（ゲインは基板の中の値。速度ループのゲイン 39〜41 とは別）
            float err = out.value - readCurrentMa();
            integral += err;
            mas_can_md_output_t duty = {MAS_CAN_MD_OUT_DUTY, false,
                                        0.0005f * err + 0.00001f * integral, false};
            motorApply(duty);
        } else {
            motorApply(out);  // STOP と DUTY はそのまま
        }
        (void)
            set;  // 電流の上限（set->current_limit_ma）を VREF に、反転（set->reverse）を向きに使う

        // 測った値を渡す（FB はコアが組み立てて送る）
        mas_can_md_measure_t m = {};
        m.current_ma = readCurrentMa();
        m.duty = 0;  // 実際に出している duty
        mas_can_md_set_measure(&node, 0, &m);

        mas_can_frame_t f;
        while (mas_can_node_pop_tx(&node, &f)) {
            if (!canSend(f)) break;  // 箱が満杯なら次のループで（取り出した 1 本は捨てる）
        }
    }
}
