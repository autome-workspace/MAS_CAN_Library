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

/* 出力は PWM の 1 周期ごとの割り込み（ADC の変換の終わり、30 kHz）で決める。main は指令を置くだけ。
 * 電流は PWM のオンの区間の終わりで測る。オフの間（低速減衰）は検出抵抗に流れないため。
 * TIM2 CH3（ピンには出さない）の PWM2 の立ち上がり ＝ TRGO で ADC を起動する。 */
#define SO_SETTLE 160 /* オンの後 10 µs は SO がリンギングする（実測）。これより短いと測れない */
#define SO_MARGIN 24  /* オフになる 1.5 µs 前に測る */
#define ADC_TO_MA_Q10 8253 /* 1 カウント ＝ 3300 / 4095 / 100 A ＝ 8.06 mA */
#define KP_Q8 8            /* 電流の PI。誤差 1 mA あたりの duty [カウント] × 256 */
#define CHOP_Q8 384        /* 打ち切りの上限 ＝ 目標 × 1.5（保険） */
#define KI_Q16 1048        /* 誤差 1 mA あたり 1 周期ごとに足す duty [カウント] × 65536 */

typedef struct {
    mas_can_md_out_mode_t mode;
    int32_t duty;      /* DUTY: 符号付きのカウント */
    int32_t target_ma; /* CURRENT */
    uint32_t vref;     /* TIM3 の比較値（DRV8701 の打ち切り） */
    bool reset;
} drive_cmd_t;

static volatile drive_cmd_t cmd;
static volatile int32_t meas_q4; /* 測った電流 [mA] × 16。出している向きを正 */
static volatile int32_t applied; /* 出している duty（符号付きのカウント） */

/* 電流の指令: 目標との差で PI を回し duty を決める。duty の下限は測れる長さ（SO_SETTLE）にして、
 * それより小さい電流は DRV8701 の打ち切り（目標の 1.5 倍）に任せる。
 * 目標 0 は下限なしで 0 に追従させ（空転の代わり）、測れなくなったら duty を少しずつ下げる */
static int32_t current_loop(int32_t target_ma, bool valid, int32_t ma, bool reset) {
    static int32_t integ; /* duty [カウント] × 65536 */
    static int32_t dir = 1;
    if (target_ma != 0 && (target_ma > 0) != (dir > 0)) {
        dir = -dir;
        reset = true;
    }
    int32_t mag = target_ma < 0 ? -target_ma : target_ma;
    int32_t floor = mag ? SO_SETTLE : 0;
    if (reset) integ = floor << 16;
    int32_t p = 0;
    if (valid) {
        int32_t err = mag - ma;
        integ += err * KI_Q16;
        p = (err * KP_Q8) >> 8;
    } else if (mag == 0) {
        integ -= integ >> 11;
    }
    if (integ < floor << 16) integ = floor << 16;
    if (integ > (int32_t)PWM_FULL_ON << 16) integ = (int32_t)PWM_FULL_ON << 16;
    int32_t d = (integ >> 16) + p;
    if (d < floor) d = floor;
    if (d > (int32_t)PWM_FULL_ON) d = PWM_FULL_ON;
    return dir * d;
}

void ADC1_IRQHandler(void) {
    static bool valid; /* この変換がオンの区間の中で取れたか（1 周期前に決めた） */
    int32_t ma = (((int32_t)ADC1->DR - (int32_t)current_zero_adc) * ADC_TO_MA_Q10) >> 10;
    if (valid) meas_q4 += ma - (meas_q4 >> 4);

    bool reset = cmd.reset;
    cmd.reset = false;
    int32_t on = 0;
    switch (cmd.mode) {
        case MAS_CAN_MD_OUT_DUTY:
            on = cmd.duty;
            break;
        case MAS_CAN_MD_OUT_CURRENT:
            on = current_loop(cmd.target_ma, valid, ma, reset);
            break;
        default:
            current_loop(0, false, 0, true);
            break;
    }

    uint32_t mag = (uint32_t)(on < 0 ? -on : on);
    if (cmd.mode != MAS_CAN_MD_OUT_DUTY && cmd.mode != MAS_CAN_MD_OUT_CURRENT)
        bridge(PWM_FULL_ON, PWM_FULL_ON); /* 止める: 空転は出せないので、いつもブレーキ */
    else if (on >= 0)
        bridge(mag, 0); /* オフの間は DRV8701P の低速減衰（ブレーキ） */
    else
        bridge(0, mag);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, cmd.vref);
    valid = mag >= SO_SETTLE;
    if (valid)
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3,
                              mag > PWM_PERIOD ? PWM_PERIOD / 2 : mag - SO_MARGIN);
    if ((on < 0) != (applied < 0) || on == 0) meas_q4 = 0;
    applied = on;
}

static void motor_command(const mas_can_md_output_t *out, const mas_can_md_settings_t *set) {
    int32_t duty = 0, target = 0;
    uint32_t vref = ma_to_vref(set->current_limit_ma);
    if (out->mode == MAS_CAN_MD_OUT_DUTY) {
        float d = out->value > 1.0f ? 1.0f : out->value < -1.0f ? -1.0f : out->value;
        duty = (int32_t)(d * PWM_FULL_ON + (d < 0 ? -0.5f : 0.5f));
    } else if (out->mode == MAS_CAN_MD_OUT_CURRENT) {
        float lim = (float)set->current_limit_ma;
        float t = out->value > lim ? lim : out->value < -lim ? -lim : out->value;
        target = (int32_t)t;
        uint32_t mag = (uint32_t)(target < 0 ? -target : target);
        if (mag) vref = ma_to_vref((mag * CHOP_Q8) >> 8);
    } else {
        vref = 0;
    }
    __disable_irq();
    cmd.mode = out->mode;
    cmd.duty = duty;
    cmd.target_ma = target;
    cmd.vref = vref;
    cmd.reset = cmd.reset || out->reset_ctrl;
    __enable_irq();
}

/* 測れない間（duty が小さい）は前の値。止めていれば 0 */
static float read_current_ma(void) {
    int32_t on = applied;
    float ma = (float)meas_q4 / 16.0f;
    return on == 0 ? 0.0f : on < 0 ? -ma : ma;
}

static float read_duty(void) { return (float)applied / (float)PWM_FULL_ON; }

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
    bridge(PWM_FULL_ON, PWM_FULL_ON);
    cmd.mode = MAS_CAN_MD_OUT_STOP;
    HAL_ADC_Start(&hadc);
    __HAL_ADC_ENABLE_IT(&hadc, ADC_IT_EOC);
    HAL_NVIC_SetPriority(ADC1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(ADC1_IRQn);

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
    motor_command(&out, mas_can_md_settings(&node, 0));

    mas_can_md_measure_t m = {0};
    m.current_ma = read_current_ma();
    m.duty = read_duty();
    mas_can_md_set_measure(&node, 0, &m);

    const mas_can_frame_t *f;
    while ((f = mas_can_node_peek_tx(&node)) && can_send(f)) mas_can_node_drop_tx(&node);

    led_update(node.cfg.md_first_ch, HAL_GetTick());
}
