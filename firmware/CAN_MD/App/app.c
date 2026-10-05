/* 2ch-MD（STM32F042、1 ch）のポート。プロトコルの処理は lib/mas の node が受け持つ。
 * md-firmware-draft.md §5。 */
#include "app.h"

#include <string.h>

#include "main.h"
#include "mas/can/node.h"

extern ADC_HandleTypeDef hadc;
extern CAN_HandleTypeDef hcan;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;

#define PWM_PERIOD 532u /* TIM2、TIM3 の周期（16 MHz で約 30 kHz） */
#define PWM_FULL_ON (PWM_PERIOD + 1u)
#define ADC_FULL 4095u
#define ADC_REF_MV 3300u
#define SENSE_MV_PER_A 100u       /* 5 mΩ × 20 V/V */
#define SETTINGS_ADDR 0x08007C00u /* 最後のページ（1 KB）。リンカスクリプトでプログラムから外す */

static mas_can_node_t node;
static uint16_t current_zero_adc;
static float duty_now;

/* ---- CAN ---- */

#define RXQ 16u
static mas_can_frame_t rxq[RXQ];
static volatile uint8_t rx_head, rx_tail;

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *h) {
    CAN_RxHeaderTypeDef hdr;
    uint8_t data[8];
    if (HAL_CAN_GetRxMessage(h, CAN_RX_FIFO0, &hdr, data) != HAL_OK || hdr.IDE != CAN_ID_STD)
        return;
    if ((uint8_t)(rx_head - rx_tail) >= RXQ) return; /* 溢れたら捨てる */
    mas_can_frame_t *f = &rxq[rx_head % RXQ];
    f->id = (uint16_t)hdr.StdId;
    f->dlc = (uint8_t)(hdr.DLC > 8 ? 8 : hdr.DLC);
    f->rtr = hdr.RTR == CAN_RTR_REMOTE;
    memcpy(f->data, data, 8);
    rx_head++;
}

static bool can_send(const mas_can_frame_t *f) {
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0) return false;
    CAN_TxHeaderTypeDef hdr = {0};
    hdr.StdId = f->id;
    hdr.IDE = CAN_ID_STD;
    hdr.RTR = f->rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    hdr.DLC = f->dlc;
    uint32_t mailbox;
    return HAL_CAN_AddTxMessage(&hcan, &hdr, (uint8_t *)f->data, &mailbox) == HAL_OK;
}

static void can_filter(uint8_t bank, uint16_t id, uint16_t mask) {
    CAN_FilterTypeDef fl = {0};
    fl.FilterBank = bank;
    fl.FilterMode = CAN_FILTERMODE_IDMASK;
    fl.FilterScale = CAN_FILTERSCALE_32BIT;
    fl.FilterIdHigh = (uint16_t)(id << 5);
    fl.FilterMaskIdHigh = (uint16_t)(mask << 5);
    fl.FilterMaskIdLow = 0x0004u; /* 標準 ID だけ（リモートフレームも通す。0x000 のため） */
    fl.FilterFIFOAssignment = CAN_RX_FIFO0;
    fl.FilterActivation = ENABLE;
    fl.SlaveStartFilterBank = 14;
    HAL_CAN_ConfigFilter(&hcan, &fl);
}

/* ---- 基板の性質 ---- */

static uint8_t read_dip(void) { /* 印字 1 ＝ bit 0（PA5）、2 ＝ PB1、3 ＝ PA4。on で 0 */
    uint8_t k = 0;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_5) == GPIO_PIN_RESET) k |= 1;
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_RESET) k |= 2;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) == GPIO_PIN_RESET) k |= 4;
    return k;
}

static uint64_t unique_id(void) { /* D22 */
    const uint32_t *w = (const uint32_t *)UID_BASE;
    return ((uint64_t)(w[2] ^ w[1]) << 32) | w[0];
}

static bool flash_load(void *ctx, void *buf, size_t len) {
    (void)ctx;
    memcpy(buf, (const void *)SETTINGS_ADDR, len);
    return true;
}

static bool flash_save(void *ctx, const void *buf, size_t len) {
    (void)ctx;
    FLASH_EraseInitTypeDef er = {FLASH_TYPEERASE_PAGES, SETTINGS_ADDR, 1};
    uint32_t err;
    bool ok = HAL_FLASH_Unlock() == HAL_OK && HAL_FLASHEx_Erase(&er, &err) == HAL_OK;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; ok && i < len; i += 2) {
        uint16_t hw = (uint16_t)(p[i] | ((i + 1 < len ? p[i + 1] : 0xFF) << 8));
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, SETTINGS_ADDR + i, hw) == HAL_OK;
    }
    HAL_FLASH_Lock();
    return ok;
}

static void restart(void *ctx) {
    (void)ctx;
    NVIC_SystemReset();
}

static uint32_t now_us(void) { return HAL_GetTick() * 1000u; }

/* ---- モーター ---- */

static uint32_t ma_to_vref(uint32_t ma) { /* DRV8701: ICHOP = (VREF - VOFF) / (AV × R) */
    uint32_t off_mv = ((uint32_t)current_zero_adc * ADC_REF_MV + ADC_FULL / 2) / ADC_FULL;
    uint32_t mv = off_mv + (ma * SENSE_MV_PER_A + 999u) / 1000u;
    uint32_t cmp = (mv * PWM_PERIOD + ADC_REF_MV / 2) / ADC_REF_MV;
    return cmp > PWM_PERIOD ? PWM_PERIOD : cmp;
}

/* 正の向きは旧ファームと同じく TIM2 CH1（PA0 ＝ IN2）を出す側 */
static void bridge(uint32_t pos, uint32_t neg) {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pos);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, neg);
}

/* 電流は PWM のオンの区間の終わりで測る。オフの間（低速減衰）は検出抵抗に流れないため。
 * TIM2 CH3（ピンには出さない）の PWM2 の立ち上がり ＝ TRGO で ADC を起動する */
#define SO_SETTLE 160u /* オンになった後 10 µs ほど SO がリンギングする（実測） */
#define SO_MARGIN 24u  /* オフになる 1.5 µs 前に測る */

static volatile bool sample_valid;
static float current_ma_avg;

static void set_sample_point(uint32_t on) {
    if (on < SO_SETTLE) {
        sample_valid = false;
        return;
    }
    uint32_t at = on > PWM_PERIOD ? PWM_PERIOD / 2 : on - SO_MARGIN;
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, at);
    sample_valid = true;
}

static void motor_apply(const mas_can_md_output_t *out, const mas_can_md_settings_t *set) {
    switch (out->mode) {
        case MAS_CAN_MD_OUT_DUTY: {
            /* IN1/IN2 の片方を PWM、もう片方を 0。オフの間は DRV8701P の低速減衰（ブレーキ） */
            float d = out->value > 1.0f ? 1.0f : out->value < -1.0f ? -1.0f : out->value;
            uint32_t cmp = (uint32_t)((d < 0 ? -d : d) * PWM_FULL_ON + 0.5f);
            __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, ma_to_vref(set->current_limit_ma));
            if (d >= 0)
                bridge(cmp, 0);
            else
                bridge(0, cmp);
            set_sample_point(cmp);
            duty_now = d;
            break;
        }
        case MAS_CAN_MD_OUT_CURRENT: {
            /* 仮: 旧ファームと同じく DRV8701 の打ち切りに任せる（F7: 後でソフトの電流ループにする）
             */
            float ma = out->value;
            uint32_t mag = (uint32_t)(ma < 0 ? -ma : ma);
            if (mag > set->current_limit_ma) mag = set->current_limit_ma;
            __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, mag ? ma_to_vref(mag) : 0);
            if (mag == 0)
                bridge(PWM_FULL_ON, PWM_FULL_ON);
            else if (ma > 0)
                bridge(PWM_FULL_ON, 0);
            else
                bridge(0, PWM_FULL_ON);
            set_sample_point(mag ? PWM_FULL_ON : 0);
            duty_now = mag == 0 ? 0.0f : (ma > 0 ? 1.0f : -1.0f);
            break;
        }
        default: /* 止める: 空転は出せないので、いつもブレーキ（IN1 ＝ IN2 ＝ 1） */
            __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0);
            bridge(PWM_FULL_ON, PWM_FULL_ON);
            set_sample_point(0);
            duty_now = 0;
            break;
    }
}

/* 測れない間（止めている、duty が小さい）は、止めていれば 0、そうでなければ前の値 */
static float read_current_ma(void) {
    if (__HAL_ADC_GET_FLAG(&hadc, ADC_FLAG_EOC)) {
        uint32_t v = HAL_ADC_GetValue(&hadc);
        if (sample_valid) {
            float ma = v > current_zero_adc ? (float)(v - current_zero_adc) * ADC_REF_MV /
                                                  ADC_FULL * 1000.0f / SENSE_MV_PER_A
                                            : 0.0f;
            current_ma_avg += (ma - current_ma_avg) * 0.0625f;
        }
    }
    if (duty_now == 0) current_ma_avg = 0;
    return duty_now < 0 ? -current_ma_avg : current_ma_avg;
}

static void start_current_sampling(void) {
    HAL_ADC_Stop(&hadc);
    hadc.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
    hadc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    HAL_ADC_Init(&hadc);
    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel = ADC_CHANNEL_6;
    ch.Rank = ADC_RANK_CHANNEL_NUMBER;
    ch.SamplingTime = ADC_SAMPLETIME_7CYCLES_5;
    HAL_ADC_ConfigChannel(&hadc, &ch);

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM2;
    oc.Pulse = PWM_PERIOD / 2;
    HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_3);
    TIM_MasterConfigTypeDef mc = {0};
    mc.MasterOutputTrigger = TIM_TRGO_OC3REF;
    mc.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim2, &mc);
}

static void calibrate_zero(void) {
    uint32_t sum = 0, n = 0;
    for (int i = 0; i < 128; i++) {
        HAL_ADC_Start(&hadc);
        if (HAL_ADC_PollForConversion(&hadc, 1) == HAL_OK) {
            sum += HAL_ADC_GetValue(&hadc);
            n++;
        }
    }
    current_zero_adc = n ? (uint16_t)(sum / n) : 0;
}

/* ---- LED: 基板の位置 k を k ＋ 1 回の点滅で示す ---- */

static void led_update(uint8_t k, uint32_t now_ms) {
    static uint32_t last;
    static uint8_t count;
    static bool on;
    if ((uint32_t)(now_ms - last) < (on ? 150u : (count == 0 ? 1000u : 150u))) return;
    last = now_ms;
    if (on) {
        on = false;
        count = (uint8_t)((count + 1) % (k + 1));
    } else {
        on = true;
    }
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ---- 入口 ---- */

void app_init(void) {
    HAL_ADCEx_Calibration_Start(&hadc);
    calibrate_zero();
    start_current_sampling();
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
    mas_can_md_output_t stop = {MAS_CAN_MD_OUT_STOP, true, 0, false};
    mas_can_md_settings_t dummy = {0};
    motor_apply(&stop, &dummy);
    HAL_ADC_Start(&hadc);

    mas_can_node_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.uid = unique_id();
    cfg.board_type = 0x00010001u;
    cfg.fw_version = 0x00000100u;       /* 0.1.0 */
    cfg.protocol_version = 0x00000000u; /* 未定（Q14） */
    cfg.md_first_ch = read_dip();
    cfg.md_count = 1;
    cfg.md_has_current = true;
    cfg.md_has_speed = false;
    cfg.md_has_coast = false;
    cfg.md_current_limit_max_ma = 10000;
    cfg.md_current_limit_default_ma = 2400;
    cfg.md_fb_rate_max_hz = 500;
    cfg.load = flash_load;
    cfg.save = flash_save;
    cfg.restart = restart;
    mas_can_node_init(&node, &cfg, now_us());

    can_filter(0, 0x000, 0x700); /* class 0 */
    can_filter(1, 0x210, 0x7F0); /* MD の電流・速度の指令 */
    can_filter(2, 0x220, 0x7F0); /* MD の duty・位置の指令 */
    can_filter(3, 0x400, 0x7FF); /* MD の設定の要求 */
    HAL_CAN_Start(&hcan);
    HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING);
    mas_can_node_ready(&node, now_us());
}

void app_loop(void) {
    uint32_t now = now_us();
    while (rx_tail != rx_head) {
        mas_can_node_on_frame(&node, &rxq[rx_tail % RXQ], now);
        rx_tail++;
    }
    mas_can_node_tick(&node, now);

    mas_can_md_output_t out = mas_can_md_output(&node, 0);
    motor_apply(&out, mas_can_md_settings(&node, 0));

    mas_can_md_measure_t m = {0};
    m.current_ma = read_current_ma();
    m.duty = duty_now;
    mas_can_md_set_measure(&node, 0, &m);

    const mas_can_frame_t *f;
    while ((f = mas_can_node_peek_tx(&node)) && can_send(f)) mas_can_node_drop_tx(&node);

    led_update(node.cfg.md_first_ch, HAL_GetTick());
}
