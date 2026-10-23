/* micro-MD（CH32V203K8T6、DRV8701P、1 ch）のポート。プロトコルの処理は lib/mas の node が受け持つ。
 * 構成は firmware/CAN_MD/App/app.c にそろえている。 */
#include <ch32v20x.h>
#include <string.h>

#include "mas/can/node.h"

/* 基板の種類（C-63）。台帳の一覧に無い仮の番号。登録したら core/board.h に移す */
#define BOARD_KIND_MICRO_MD 0x0003u
#define FW_VERSION 0x00000100u /* 0.1.0 */

#ifndef MICRO_MD_CH
#define MICRO_MD_CH 0 /* k を保存していないときの ch */
#endif

#define CURRENT_LIMIT_MAX_MA 10000u /* ハードウェアの上限（旧ファームの VREF_CURRENT_LIMIT_MA） */
#define PWM_TOP 6000u               /* TIM2、TIM3 の 1 周期（120 MHz で 20 kHz）。この値で常にオン */
#define ADC_FULL 4095u
#define ADC_REF_MV 3300u
#define SENSE_MV_PER_A 100u /* 5 mΩ × 20 V/V */

/* 最後の 1 KB（link.ld でプログラムから外してある）。高速消去の 1 ページは 256 byte */
#define SETTINGS_ADDR 0x0800FC00u /* node の保存した設定 */
#define K_ADDR 0x0800FD00u        /* 基板の位置 k */
#define K_MAGIC 0x4D4B0000u       /* "MK" ＋ k */
#define UID_ADDR 0x1FFFF7E8u

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void ADC1_2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

static mas_can_node_t node;
static uint16_t current_zero_adc;

/* ---- 時計 ---- */

/* SystemInit は HSI のまま返す（platformio.ini）。16 MHz のセラロック ÷ 2 × 15 ＝ 120 MHz。
 * 起動しなければ HSI × 15 で同じ周波数にする（CAN の精度は足りない）。
 * PCLK1 ＝ 60 MHz（CAN）、PCLK2 ＝ 60 MHz、ADC ＝ 10 MHz（上限 14 MHz）、TIM2〜4 ＝ 120 MHz */
static bool clock_init(void) {
    RCC->CTLR |= RCC_HSEON;
    for (uint32_t n = 0; n < 0x40000u && !(RCC->CTLR & RCC_HSERDY); n++) {
    }
    bool hse = (RCC->CTLR & RCC_HSERDY) != 0;
    uint32_t src = RCC_PLLSRC_HSE | RCC_PLLXTPRE_HSE_Div2;
    if (!hse) {
        RCC->CTLR &= ~RCC_HSEON;
        EXTEN->EXTEN_CTR |= EXTEN_PLL_HSI_PRE; /* HSI を割らずに PLL へ */
        src = RCC_PLLSRC_HSI_Div2;
    }
    RCC->CFGR0 &= ~(RCC_HPRE | RCC_PPRE1 | RCC_PPRE2 | RCC_ADCPRE | RCC_PLLSRC | RCC_PLLXTPRE |
                    RCC_PLLMULL);
    RCC->CFGR0 |= RCC_HPRE_DIV1 | RCC_PPRE1_DIV2 | RCC_PPRE2_DIV2 | RCC_ADCPRE_DIV6 | src |
                  RCC_PLLMULL15;
    RCC->CTLR |= RCC_PLLON;
    while (!(RCC->CTLR & RCC_PLLRDY)) {
    }
    RCC->CFGR0 = (RCC->CFGR0 & ~RCC_SW) | RCC_SW_PLL;
    while ((RCC->CFGR0 & RCC_SWS) != RCC_SWS_PLL) {
    }
    SystemCoreClockUpdate();
    return hse;
}

/* TIM4 を 1 MHz で回し、16 bit の一周を main ループで数えて 32 bit にする */
static uint32_t now_us(void) {
    static uint32_t high;
    static uint16_t last;
    uint16_t c = TIM4->CNT;
    if (c < last) high += 0x10000u;
    last = c;
    return high | c;
}

/* ---- CAN ---- */

#define RXQ 16u
static mas_can_frame_t rxq[RXQ];
static volatile uint8_t rx_head, rx_tail;

void USB_LP_CAN1_RX0_IRQHandler(void) {
    while (CAN_MessagePending(CAN1, CAN_FIFO0)) {
        CanRxMsg m;
        CAN_Receive(CAN1, CAN_FIFO0, &m);
        if (m.IDE != CAN_Id_Standard) continue;
        if ((uint8_t)(rx_head - rx_tail) >= RXQ) continue; /* 溢れたら捨てる */
        mas_can_frame_t *f = &rxq[rx_head % RXQ];
        f->id = (uint16_t)m.StdId;
        f->dlc = (uint8_t)(m.DLC > 8 ? 8 : m.DLC);
        f->rtr = m.RTR == CAN_RTR_Remote;
        memcpy(f->data, m.Data, 8);
        rx_head++;
    }
}

static bool can_send(const mas_can_frame_t *f) {
    CanTxMsg m = {0};
    m.StdId = f->id;
    m.IDE = CAN_Id_Standard;
    m.RTR = f->rtr ? CAN_RTR_Remote : CAN_RTR_Data;
    m.DLC = f->dlc;
    memcpy(m.Data, f->data, 8);
    return CAN_Transmit(CAN1, &m) != CAN_TxStatus_NoMailBox;
}

static void can_filter(uint8_t bank, uint16_t id, uint16_t mask) {
    CAN_FilterInitTypeDef fl = {0};
    fl.CAN_FilterNumber = bank;
    fl.CAN_FilterMode = CAN_FilterMode_IdMask;
    fl.CAN_FilterScale = CAN_FilterScale_32bit;
    fl.CAN_FilterIdHigh = (uint16_t)(id << 5);
    fl.CAN_FilterMaskIdHigh = (uint16_t)(mask << 5);
    fl.CAN_FilterMaskIdLow = 0x0004u; /* 標準 ID だけ（リモートフレームも通す。0x000 のため） */
    fl.CAN_FilterFIFOAssignment = CAN_Filter_FIFO0;
    fl.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&fl);
}

/* 1 Mbps: 60 MHz ÷ 3 ＝ 20 TQ（1 ＋ 15 ＋ 4）、サンプル点 80%、SJW 4 TQ（C-9） */
static void can_init(void) {
    GPIO_InitTypeDef io = {0};
    io.GPIO_Pin = GPIO_Pin_11; /* RX */
    io.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &io);
    io.GPIO_Pin = GPIO_Pin_12; /* TX */
    io.GPIO_Mode = GPIO_Mode_AF_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &io);

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_CAN1, ENABLE);
    CAN_InitTypeDef c = {0};
    c.CAN_ABOM = ENABLE; /* バスオフからの自動復帰（C-68） */
    c.CAN_Mode = CAN_Mode_Normal;
    c.CAN_SJW = CAN_SJW_4tq;
    c.CAN_BS1 = CAN_BS1_15tq;
    c.CAN_BS2 = CAN_BS2_4tq;
    c.CAN_Prescaler = 3;
    CAN_Init(CAN1, &c);

    can_filter(0, 0x000, 0x7FC); /* E-stop、解除、SYNC */
    can_filter(1, 0x210, 0x7F0); /* MD の電流・速度の指令 */
    can_filter(2, 0x220, 0x7F0); /* MD の duty・位置の指令 */
    can_filter(3, 0x400, 0x7FF); /* MD の設定の要求 */

    CAN_ITConfig(CAN1, CAN_IT_FMP0, ENABLE);
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 0x80);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
}

/* 立っていたバスオフが消えたら、自動復帰したとして異常の詳細の bit 2 を立てる */
static void watch_bus_off(void) {
    static bool was_off;
    bool off = CAN_GetFlagStatus(CAN1, CAN_FLAG_BOF) == SET;
    if (was_off && !off) mas_can_node_raise_fault(&node, MAS_CAN_FAULT_BUS_OFF);
    was_off = off;
}

/* ---- フラッシュ ---- */

static void motor_brake_now(void);

/* 256 byte のページ 1 枚を消して書く。消去と書き込みの間は割り込みが PWM を変えられないことが
 * あるので、先にブレーキにしておく（次の main ループで指令に戻る） */
static bool flash_write_page(uint32_t addr, const void *buf, size_t len) {
    static uint32_t page[64];
    if (len > sizeof(page)) return false;
    memset(page, 0xFF, sizeof(page));
    memcpy(page, buf, len);
    motor_brake_now();
    FLASH_Unlock_Fast();
    FLASH_ErasePage_Fast(addr);
    FLASH_ProgramPage_Fast(addr, page);
    FLASH_Lock_Fast();
    FLASH_Lock();
    /* 読み返しでは確かめない（書いた内容は再起動するまでゼロウェイトの写しに映らないことがある） */
    return !(FLASH->STATR & FLASH_FLAG_WRPRTERR);
}

static bool flash_load(void *ctx, void *buf, size_t len) {
    (void)ctx;
    memcpy(buf, (const void *)SETTINGS_ADDR, len);
    return true;
}

static bool flash_save(void *ctx, const void *buf, size_t len) {
    (void)ctx;
    return flash_write_page(SETTINGS_ADDR, buf, len);
}

static uint8_t load_k(void) {
    uint32_t w = *(const uint32_t *)K_ADDR;
    uint8_t k = (uint8_t)w;
    return (w & 0xFFFFFF00u) == K_MAGIC && k < MAS_CAN_MD_MAX_CH ? k : MICRO_MD_CH;
}

static void save_k(uint8_t k) {
    uint32_t w = K_MAGIC | k;
    flash_write_page(K_ADDR, &w, sizeof(w));
}

/* 送信の箱が空く（応答を送り終える）のを少し待ってから再起動する（C-66） */
static void restart(void *ctx) {
    (void)ctx;
    uint32_t t0 = now_us();
    while ((CAN1->TSTATR & (CAN_TSTATR_TME0 | CAN_TSTATR_TME1 | CAN_TSTATR_TME2)) !=
               (CAN_TSTATR_TME0 | CAN_TSTATR_TME1 | CAN_TSTATR_TME2) &&
           (uint32_t)(now_us() - t0) < 10000u) {
    }
    NVIC_SystemReset();
}

static uint64_t unique_id(void) { /* C-62 */
    const uint32_t *w = (const uint32_t *)UID_ADDR;
    return ((uint64_t)(w[2] ^ w[1]) << 32) | w[0];
}

/* ---- モーター ---- */

/* DRV8701P: IN1 ＝ PB4（TIM3 CH1）、IN2 ＝ PB5（TIM3 CH2）。00 と 11 はどちらもブレーキで、
 * nSLEEP が 3.3V 直結なので空転は出せない。正の向きは IN1 を出す側（旧ファームと同じ）。
 * 出力は PWM の 1 周期ごとの割り込み（ADC の変換の終わり、20 kHz）で決める。main は指令を置くだけ。
 * 片側だけを PWM し、オフの間は 00（低速減衰）。
 *
 * 電流は PWM のオンの区間の終わりで測る。オフの間（低速減衰）は検出抵抗に流れないため。
 * TIM3 CH3（ピンには出さない）の PWM2 の立ち上がり ＝ TRGO で ADC を起動する。
 * 電流の指令は、測った電流で PI を回して duty を決める（CAN_MD と同じ）。DRV8701 の打ち切り（VREF。
 * PB3、TIM2 CH2 を R11/C12 で均す）は保険で、duty では上限（33）、電流では目標の 1.5 倍に置く。
 * 打ち切りに任せると、打ち切りが PWM と無関係な時刻に入り、SO の揺れを測ってしまう（実機）。 */
#ifndef SO_SETTLE
#define SO_SETTLE 1200 /* オンの後 10 µs は SO が揺れる（CAN_MD の実測） */
#endif
#define SO_MARGIN 180 /* オフになる 1.5 µs 前に測る */
#define SO_MIN_ON (SO_SETTLE + SO_MARGIN) /* 測る時刻が SO_SETTLE より後になる最短のオン */
#define ADC_TO_MA_Q10 8253 /* 1 カウント ＝ 3300 / 4095 / 100 A ＝ 8.06 mA */
/* 電流の PI（CAN_MD の値を周期 6000 カウント、20 kHz に換算） */
#ifndef KP_Q8
#define KP_Q8 90 /* 誤差 1 mA あたりの duty [カウント] × 256 */
#endif
#ifndef KI_Q16
#define KI_Q16 17700 /* 誤差 1 mA あたり 1 周期ごとに足す duty [カウント] × 65536 */
#endif
#define CHOP_Q8 384 /* 打ち切り ＝ 目標 × 1.5 */
/* 音: 向きを 2 kHz で入れ替えてコイルを鳴らす（平均の力は 0）。基板にブザーが無いため */
#define BEEP_HALF 5        /* 半周期 ＝ 5 × 50 µs */
#define BEEP_ON 600        /* duty 10% */
#define BEEP_LIMIT_MA 1000 /* 鳴らす間の VREF */

typedef struct {
    mas_can_md_out_mode_t mode;
    int32_t duty;      /* DUTY: 符号付きのオンのカウント（PWM_TOP で常にオン） */
    int32_t target_ma; /* CURRENT */
    uint32_t vref;     /* TIM2 の比較値 */
    bool reset;
} drive_cmd_t;

static volatile drive_cmd_t cmd;
static volatile int32_t meas_q4; /* 測った電流 [mA] × 16。出している向きを正 */
static volatile int32_t applied; /* 出しているオンのカウント（符号付き） */
static volatile bool beeping;

static void bridge(uint32_t in1, uint32_t in2) {
    TIM3->CH1CVR = in1;
    TIM3->CH2CVR = in2;
}

/* 電流の指令: 目標との差で PI を回し duty を決める。duty の下限は測れる長さ（SO_MIN_ON）にして、
 * それより小さい電流は打ち切り（目標の 1.5 倍）に任せる。
 * 目標 0 は下限なしで 0 に追従させ、測れなくなったら duty を少しずつ下げる */
static int32_t current_loop(int32_t target_ma, bool valid, int32_t ma, bool reset) {
    static int32_t integ; /* duty [カウント] × 65536 */
    static int32_t dir = 1;
    if (target_ma != 0 && (target_ma > 0) != (dir > 0)) {
        dir = -dir;
        reset = true;
    }
    int32_t mag = target_ma < 0 ? -target_ma : target_ma;
    int32_t floor = mag ? SO_MIN_ON : 0;
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
    if (integ > (int32_t)PWM_TOP << 16) integ = (int32_t)PWM_TOP << 16;
    int32_t d = (integ >> 16) + p;
    if (d < floor) d = floor;
    if (d > (int32_t)PWM_TOP) d = PWM_TOP;
    return dir * d;
}

void ADC1_2_IRQHandler(void) {
    static bool valid; /* この変換がオンの区間の中で取れたか（1 周期前に決めた） */
    int32_t ma = (((int32_t)ADC1->RDATAR - (int32_t)current_zero_adc) * ADC_TO_MA_Q10) >> 10;
    if (valid) meas_q4 += ma - (meas_q4 >> 4);

    bool reset = cmd.reset;
    cmd.reset = false;
    int32_t on = 0;
    if (beeping) {
        static uint8_t t;
        t = (uint8_t)((t + 1) % (2 * BEEP_HALF));
        on = t < BEEP_HALF ? BEEP_ON : -BEEP_ON;
    } else if (cmd.mode == MAS_CAN_MD_OUT_DUTY) {
        on = cmd.duty;
    } else if (cmd.mode == MAS_CAN_MD_OUT_CURRENT) {
        on = current_loop(cmd.target_ma, valid, ma, reset);
    } else {
        current_loop(0, false, 0, true);
    }

    uint32_t mag = (uint32_t)(on < 0 ? -on : on);
    if (on >= 0)
        bridge(mag, 0);
    else
        bridge(0, mag);
    TIM2->CH2CVR = cmd.vref;
    valid = mag >= SO_MIN_ON;
    TIM3->CH3CVR = valid && mag < PWM_TOP ? mag - SO_MARGIN : PWM_TOP / 2;
    if ((on < 0) != (applied < 0) || on == 0) meas_q4 = 0;
    applied = on;
}

static uint32_t ma_to_vref(uint32_t ma) { /* DRV8701: ICHOP = (VREF - VOFF) / (AV × R) */
    uint32_t off_mv = ((uint32_t)current_zero_adc * ADC_REF_MV + ADC_FULL / 2) / ADC_FULL;
    uint32_t mv = off_mv + (ma * SENSE_MV_PER_A + 999u) / 1000u;
    uint32_t c = (mv * PWM_TOP + ADC_REF_MV / 2) / ADC_REF_MV;
    return c > PWM_TOP ? PWM_TOP : c;
}

/* 鳴らして待つ（main から。その間 node は回らない） */
static void beep(uint32_t on_ms, uint32_t off_ms) {
    cmd.mode = MAS_CAN_MD_OUT_STOP;
    cmd.vref = ma_to_vref(BEEP_LIMIT_MA);
    beeping = true;
    for (uint32_t t0 = now_us(); (uint32_t)(now_us() - t0) < on_ms * 1000u;) {
    }
    beeping = false;
    for (uint32_t t0 = now_us(); (uint32_t)(now_us() - t0) < off_ms * 1000u;) {
    }
}

static void motor_brake_now(void) {
    __disable_irq();
    cmd.mode = MAS_CAN_MD_OUT_STOP;
    cmd.reset = true;
    bridge(0, 0);
    applied = 0;
    __enable_irq();
}

/* 指令を置く。電流の上限に当たっているかを返す（M-28） */
static bool motor_command(const mas_can_md_output_t *out, const mas_can_md_settings_t *set) {
    uint32_t lim = set->current_limit_ma;
    int32_t duty = 0, target = 0;
    uint32_t vref = ma_to_vref(lim);
    bool limited = false;
    if (out->mode == MAS_CAN_MD_OUT_DUTY) {
        float d = out->value > 1.0f ? 1.0f : out->value < -1.0f ? -1.0f : out->value;
        duty = (int32_t)(d * PWM_TOP + (d < 0 ? -0.5f : 0.5f));
        int32_t m = meas_q4 / 16;
        limited = duty != 0 && (uint32_t)(m < 0 ? 0 : m) * 10u >= lim * 9u; /* 打ち切りに届いている */
    } else if (out->mode == MAS_CAN_MD_OUT_CURRENT) {
        float a = out->value < 0 ? -out->value : out->value;
        limited = a > (float)lim;
        uint32_t mag = limited ? lim : (uint32_t)(a + 0.5f);
        target = out->value < 0 ? -(int32_t)mag : (int32_t)mag;
        if (mag) {
            uint32_t chop = (mag * CHOP_Q8) >> 8;
            vref = ma_to_vref(chop < lim ? chop : lim);
        }
    }
    __disable_irq();
    cmd.mode = out->mode;
    cmd.duty = duty;
    cmd.target_ma = target;
    cmd.vref = vref;
    cmd.reset = cmd.reset || out->reset_ctrl;
    __enable_irq();
    return limited;
}

/* 測れない間（duty が小さい）は前の値。止めていれば 0 */
static float read_current_ma(void) {
    int32_t on = applied;
    float ma = (float)meas_q4 / 16.0f;
    return on == 0 ? 0.0f : on < 0 ? -ma : ma;
}

static float read_duty(void) { return (float)applied / (float)PWM_TOP; }

static void motor_init(void) {
    GPIO_InitTypeDef io = {0};
    GPIO_PinRemapConfig(GPIO_PartialRemap_TIM3, ENABLE);  /* CH1、CH2 → PB4、PB5 */
    GPIO_PinRemapConfig(GPIO_PartialRemap1_TIM2, ENABLE); /* CH2 → PB3（CH1 の PA15 は使わない） */
    io.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5;
    io.GPIO_Mode = GPIO_Mode_AF_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &io);
    io.GPIO_Pin = GPIO_Pin_1; /* SO */
    io.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &io);

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2 | RCC_APB1Periph_TIM3, ENABLE);
    TIM_TimeBaseInitTypeDef tb = {0};
    tb.TIM_Period = PWM_TOP - 1;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &tb);
    TIM_TimeBaseInit(TIM3, &tb);
    TIM_OCInitTypeDef oc = {0};
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC2Init(TIM2, &oc);
    TIM_OC1Init(TIM3, &oc);
    TIM_OC2Init(TIM3, &oc);
    oc.TIM_OCMode = TIM_OCMode_PWM2;
    oc.TIM_OutputState = TIM_OutputState_Disable;
    oc.TIM_Pulse = PWM_TOP / 2;
    TIM_OC3Init(TIM3, &oc);
    TIM_OC2PreloadConfig(TIM2, TIM_OCPreload_Enable);
    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_SelectOutputTrigger(TIM3, TIM_TRGOSource_OC3Ref);
    bridge(0, 0);
    TIM2->CH2CVR = 0;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    ADC_InitTypeDef a = {0};
    a.ADC_Mode = ADC_Mode_Independent;
    a.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    a.ADC_DataAlign = ADC_DataAlign_Right;
    a.ADC_NbrOfChannel = 1;
    a.ADC_OutputBuffer = ADC_OutputBuffer_Disable;
    a.ADC_Pga = ADC_Pga_1;
    ADC_Init(ADC1, &a);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 1, ADC_SampleTime_7Cycles5);
    ADC_Cmd(ADC1, ENABLE);
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1)) {
    }
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1)) {
    }

    /* 止めている間の SO ＝ DRV8701 のオフセット（VOFF）。電流の 0 と VREF の足し分に使う */
    uint32_t sum = 0;
    for (int i = 0; i < 128; i++) {
        ADC_SoftwareStartConvCmd(ADC1, ENABLE);
        while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC)) {
        }
        sum += ADC_GetConversionValue(ADC1);
    }
    current_zero_adc = (uint16_t)(sum / 128);

    ADC1->CTLR2 = (ADC1->CTLR2 & ~ADC_EXTSEL) | ADC_ExternalTrigConv_T3_TRGO;
    ADC_ExternalTrigConvCmd(ADC1, ENABLE);
    ADC_ITConfig(ADC1, ADC_IT_EOC, ENABLE);
    NVIC_SetPriority(ADC1_2_IRQn, 0x00); /* 最優先 */
    NVIC_EnableIRQ(ADC1_2_IRQn);

    cmd.mode = MAS_CAN_MD_OUT_STOP;
    TIM_Cmd(TIM2, ENABLE);
    TIM_Cmd(TIM3, ENABLE);
}

/* ---- LED（PA4）とスイッチ（PA15）: 基板の位置 k（C-75） ----
 * いつもは k 回の点滅と 1 s の休み（k ＝ 0 は消えたまま。CAN_MD と同じ）。
 * 1.5 s 長押しで設定に入る（出力を止め、LED は点きっぱなし）。短く押した回数が k になり、
 * 押すたびに LED が一瞬消える。もう一度 1.5 s 長押しで保存して再起動する（k は起動時に読む。C-18）。
 * 10 s 押さなければ何もせずに戻る。 */
#define DEBOUNCE_MS 20u
#define HOLD_MS 1500u
#define SETUP_TIMEOUT_MS 10000u

static bool setup_active;

static void led(bool on) { GPIO_WriteBit(GPIOA, GPIO_Pin_4, on ? Bit_SET : Bit_RESET); }

static void led_blink_k(uint8_t k, uint32_t now_ms) {
    static uint32_t last;
    static uint8_t count;
    static bool on;
    if (k == 0) {
        led(false);
        return;
    }
    if ((uint32_t)(now_ms - last) < (on ? 150u : (count == 0 ? 1000u : 150u))) return;
    last = now_ms;
    if (on) {
        on = false;
        count = (uint8_t)((count + 1) % k);
    } else {
        on = true;
    }
    led(on);
}

static void ui_update(uint8_t k, uint32_t now_ms) {
    static bool raw_last, pressed, hold_done;
    static uint32_t raw_ms, press_ms, last_ms;
    static uint8_t count;
    bool raw = GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_15) == Bit_RESET;
    if (raw != raw_last) {
        raw_last = raw;
        raw_ms = now_ms;
    }
    if (raw != pressed && (uint32_t)(now_ms - raw_ms) >= DEBOUNCE_MS) {
        pressed = raw;
        if (pressed) {
            press_ms = now_ms;
        } else if (setup_active && !hold_done) {
            if (count < MAS_CAN_MD_MAX_CH - 1) count++;
            last_ms = now_ms;
        }
        if (!pressed) hold_done = false;
    }
    if (pressed && !hold_done && (uint32_t)(now_ms - press_ms) >= HOLD_MS) {
        hold_done = true;
        if (!setup_active) {
            setup_active = true;
            count = 0;
            last_ms = now_ms;
        } else {
            save_k(count);
            led(false);
            beep(80, 60); /* ピピッ */
            beep(80, 100);
            NVIC_SystemReset();
        }
    }
    if (setup_active && !pressed && (uint32_t)(now_ms - last_ms) >= SETUP_TIMEOUT_MS)
        setup_active = false;

    if (setup_active)
        led(!(pressed && !hold_done) && (uint32_t)(now_ms - last_ms) >= 100u);
    else
        led_blink_k(k, now_ms);
}

/* ---- 入口 ---- */

int main(void) {
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    clock_init();

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO,
                           ENABLE);
    GPIO_InitTypeDef io = {0};
    io.GPIO_Pin = GPIO_Pin_4; /* LED */
    io.GPIO_Mode = GPIO_Mode_Out_PP;
    io.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &io);
    led(false);
    io.GPIO_Pin = GPIO_Pin_15; /* スイッチ（押すと GND） */
    io.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &io);

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);
    TIM_TimeBaseInitTypeDef tb = {0};
    tb.TIM_Prescaler = 120 - 1; /* 1 MHz */
    tb.TIM_Period = 0xFFFF;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM4, &tb);
    TIM_Cmd(TIM4, ENABLE);

    motor_init();

    mas_can_node_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.uid = unique_id();
    cfg.board_type = MAS_CAN_BOARD_TYPE(BOARD_KIND_MICRO_MD, 1);
    cfg.fw_version = FW_VERSION;
    cfg.md_first_ch = load_k();
    cfg.md_count = 1;
    cfg.md_has_current = true;
    cfg.md_has_speed = false;
    cfg.md_has_coast = false;
    cfg.md_current_limit_max_ma = CURRENT_LIMIT_MAX_MA;
    cfg.md_current_limit_default_ma = CURRENT_LIMIT_MAX_MA;
    cfg.md_fb_rate_max_hz = 500;
    cfg.load = flash_load;
    cfg.save = flash_save;
    cfg.restart = restart;
    mas_can_node_init(&node, &cfg, now_us());

    can_init();
    mas_can_node_ready(&node, now_us());

    uint32_t ms = 0, ms_us = now_us();
    for (;;) {
        uint32_t now = now_us();
        while ((uint32_t)(now - ms_us) >= 1000u) {
            ms++;
            ms_us += 1000u;
        }
        while (rx_tail != rx_head) {
            mas_can_node_on_frame(&node, &rxq[rx_tail % RXQ], now);
            rx_tail++;
        }
        watch_bus_off();
        mas_can_node_tick(&node, now);

        mas_can_md_output_t out = mas_can_md_output(&node, 0);
        if (setup_active) out.mode = MAS_CAN_MD_OUT_STOP;
        bool limited = motor_command(&out, mas_can_md_settings(&node, 0));

        mas_can_md_measure_t m = {0};
        m.current_ma = read_current_ma();
        m.duty = read_duty();
        m.current_limited = limited;
        m.protecting = setup_active;
        mas_can_md_set_measure(&node, 0, &m);

        const mas_can_frame_t *f;
        while ((f = mas_can_node_peek_tx(&node)) && can_send(f)) mas_can_node_drop_tx(&node);

        ui_update(node.cfg.md_first_ch, ms);
    }
}

void NMI_Handler(void) {}

void HardFault_Handler(void) {
    bridge(0, 0);
    for (;;) {
    }
}
