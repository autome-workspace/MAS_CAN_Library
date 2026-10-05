/* 基板側の処理（純粋な C）。ポートから受けたフレームと時刻を渡すと、送るフレームと
 * 「モーターに何を出すべきか」を返す。ハードウェアには触らない。
 *
 * 使い方（ポートの main ループ）:
 *   mas_can_node_init(&node, &cfg, now);
 *   ... 準備ができたら mas_can_node_ready(&node);       // 起動の知らせ（D30）
 *   loop:
 *     受けたフレームごとに mas_can_node_on_frame(&node, &f, now);
 *     mas_can_node_tick(&node, now);
 *     out = mas_can_md_output(&node, 0) を見てモーターを動かす
 *     測った値を mas_can_md_set_measure(&node, 0, &m);
 *     while (mas_can_node_pop_tx(&node, &f)) CAN に送る
 *
 * 時刻はすべて µs（uint32_t、一周してよい）。単位は基板の中の単位（mA、rpm）。
 */
#ifndef MAS_CAN_NODE_H
#define MAS_CAN_NODE_H

#include <stddef.h>

#include "core/md.h"
#include "core/setting.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAS_CAN_NODE_MD_MAX 4 /* 1 枚の基板が受け持てる MD の ch の数（RAM のため固定） */
#define MAS_CAN_NODE_TXQ 8    /* 送る待ち行列の長さ */

/* ---- 基板の性質（ポートが起動時に渡す） ---- */
typedef struct {
    uint64_t uid;              /* 固有 ID（D22） */
    uint32_t board_type;       /* 上位 16 bit ＝ 種類、下位 16 bit ＝ 版（A5、D34） */
    uint32_t fw_version;       /* 0x00MMmmpp */
    uint32_t protocol_version; /* 0x00MMmmpp */

    uint8_t md_first_ch; /* 受け持つ MD の先頭の ch（DIP から） */
    uint8_t md_count;    /* 受け持つ MD の数（0〜MAS_CAN_NODE_MD_MAX） */
    bool md_has_current; /* 電流を測れる */
    bool md_has_speed;   /* 速度と位置を測れる */
    bool md_has_coast;   /* 空転を出せる（出せなければ止め方 34 は 1 に丸める） */

    /* 能力の上限（D19: 超える値は抑えて受け付ける） */
    uint32_t md_current_limit_max_ma; /* 電流の上限（設定 33）の最大 */
    uint32_t md_fb_rate_max_hz;       /* FB の周期（設定 35）の最大 */

    /* 焼いた値（設定の既定値）のうち、基板ごとに違うもの */
    uint32_t md_current_limit_default_ma;
    uint32_t md_encoder_cpr_default;

    /* 保存した設定の読み書き（フラッシュ）。成功で true。ctx はそのまま渡される */
    bool (*load)(void *ctx, void *buf, size_t len);
    bool (*save)(void *ctx, const void *buf, size_t len);
    void (*restart)(void *ctx); /* 設定 6（再起動）。応答を送り終えてから呼ばれる（D28） */
    void *ctx;
} mas_can_node_config_t;

/* ---- MD: コアが決めた「出すべきもの」 ---- */
typedef enum {
    MAS_CAN_MD_OUT_STOP,    /* 止める。brake で止め方を示す */
    MAS_CAN_MD_OUT_DUTY,    /* value ＝ duty（-1.0〜+1.0）。brake ＝ オフの間をブレーキに */
    MAS_CAN_MD_OUT_CURRENT, /* value ＝ 電流 [mA] */
    MAS_CAN_MD_OUT_SPEED,   /* value ＝ 速度 [rpm]（出力軸） */
} mas_can_md_out_mode_t;

typedef struct {
    mas_can_md_out_mode_t mode;
    bool brake;
    float value;
    bool reset_ctrl; /* 止めていた状態から動き出す、または種類が変わった（D21、D23）。読んだら消える
                      */
} mas_can_md_output_t;

/* ---- MD: ポートが測った値 ---- */
typedef struct {
    float current_ma;     /* md_has_current のときだけ意味がある */
    float speed_rpm;      /* md_has_speed のときだけ */
    uint32_t position;    /* 24 bit。出力軸 1 回転 ＝ 65536 */
    float duty;           /* 実際に出している duty（-1.0〜+1.0） */
    bool current_limited; /* 電流の上限に当たっている */
    bool protecting;      /* 基板の保護で止めている（D18） */
} mas_can_md_measure_t;

/* ---- MD: 設定項目の今の値（ポートが制御に使う。B1） ---- */
typedef struct {
    bool reverse;                       /* 32 */
    uint32_t current_limit_ma;          /* 33 */
    bool stop_brake;                    /* 34 */
    uint32_t fb_rate_hz;                /* 35 */
    uint32_t encoder_cpr;               /* 36 */
    float gear_ratio;                   /* 37 */
    bool encoder_reverse;               /* 38 */
    float speed_kp, speed_ki, speed_kd; /* 39〜41（mA/rpm など） */
    float max_speed_rpm;                /* 45（0 ＝ 制限なし） */
    float max_accel_rpm_s;              /* 46（0 ＝ 制限なし） */
    float current_lsb_ma;               /* 48（保存しない） */
    float speed_lsb_rpm;                /* 49（保存しない） */
} mas_can_md_settings_t;

/* ---- 中身（静的に置けるよう公開しているが、直接触らない） ---- */
typedef struct {
    mas_can_md_settings_t set;
    mas_can_md_output_t out;
    mas_can_md_measure_t meas;
    uint8_t mode;         /* 最後に受けた指令の種類（FB の bit 6–7） */
    bool commanded;       /* 起動または止めた後に、有効な指令を受けたか */
    bool unsupported;     /* こなせない指令で止めている（D10） */
    uint32_t last_cmd_us; /* 最後に有効な指令を受けた時刻 */
    uint32_t last_fb_us;  /* 最後に FB を送った時刻 */
} mas_can_node_md_t;

typedef struct {
    mas_can_node_config_t cfg;
    mas_can_node_md_t md[MAS_CAN_NODE_MD_MAX];
    uint32_t fault; /* 異常の詳細（設定 5） */
    bool estop;     /* E-stop でラッチ中 */
    bool synced;    /* 最初の SYNC を受けた */
    uint32_t last_sync_us;
    bool ready; /* 起動の知らせを送った後か（FB はその後から。D30） */
    bool restart_pending;
    mas_can_frame_t txq[MAS_CAN_NODE_TXQ];
    uint8_t txq_head, txq_len;
} mas_can_node_t;

/* ---- 共通 ---- */
void mas_can_node_init(mas_can_node_t *node, const mas_can_node_config_t *cfg, uint32_t now_us);
void mas_can_node_ready(mas_can_node_t *node, uint32_t now_us);
void mas_can_node_on_frame(mas_can_node_t *node, const mas_can_frame_t *f, uint32_t now_us);
void mas_can_node_tick(mas_can_node_t *node, uint32_t now_us);
bool mas_can_node_pop_tx(mas_can_node_t *node, mas_can_frame_t *out);

/* 基板が見つけた異常の bit を立てる（異常の詳細。種類ごとの bit 8〜など）。 */
void mas_can_node_raise_fault(mas_can_node_t *node, uint32_t bits);

/* ---- MD ---- */
/* local は基板の中の番号（0〜md_count-1）。ch ＝ md_first_ch ＋ local。 */
mas_can_md_output_t mas_can_md_output(mas_can_node_t *node, uint8_t local);
void mas_can_md_set_measure(mas_can_node_t *node, uint8_t local, const mas_can_md_measure_t *m);
const mas_can_md_settings_t *mas_can_md_settings(const mas_can_node_t *node, uint8_t local);

#ifdef __cplusplus
}
#endif

#endif /* MAS_CAN_NODE_H */
