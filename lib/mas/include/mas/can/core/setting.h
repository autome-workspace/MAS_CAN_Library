/* 設定のフレーム（class 4）と class 0 のフレームの組み立てと解読（純粋な C）。 */
#ifndef MAS_CAN_CORE_SETTING_H
#define MAS_CAN_CORE_SETTING_H

#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- class 0 ---- */

/* E-stop か（DLC もリモートフレームかも問わない。D35）。 */
bool mas_can_is_estop(const mas_can_frame_t *f);

/* 正しい E-stop の解除か（DLC 3 ちょうど、"clr"、データフレーム。D25）。 */
bool mas_can_is_estop_clear(const mas_can_frame_t *f);
void mas_can_estop_clear_encode(mas_can_frame_t *f);

/* SYNC（DLC 4 だけを受け付ける。D24）。 */
void mas_can_sync_encode(uint32_t time_us, mas_can_frame_t *f);
bool mas_can_sync_decode(const mas_can_frame_t *f, uint32_t *time_us);

/* 起動の知らせ（DLC 0）。 */
void mas_can_boot_encode(mas_can_kind_t kind, mas_can_frame_t *f);
bool mas_can_boot_decode(const mas_can_frame_t *f, mas_can_kind_t *kind);

/* ---- 設定（class 4） ---- */

typedef enum {
    MAS_CAN_OP_READ = 0,
    MAS_CAN_OP_WRITE = 1,
    MAS_CAN_OP_SAVE = 2,
    MAS_CAN_OP_INIT = 3,
} mas_can_op_t;

typedef enum {
    MAS_CAN_RES_OK = 0,
    MAS_CAN_RES_UNKNOWN_ITEM = 1,
    MAS_CAN_RES_NOT_ALLOWED = 2,
    MAS_CAN_RES_OUT_OF_RANGE = 3,
    MAS_CAN_RES_NOT_REALIZABLE = 4,
    MAS_CAN_RES_OTHER = 63,
} mas_can_result_t;

/* 共通の項目（spec-draft.md §6.7） */
#define MAS_CAN_ITEM_CHECK 0
#define MAS_CAN_ITEM_BOARD_TYPE 1
#define MAS_CAN_ITEM_FW_VERSION 2
#define MAS_CAN_ITEM_PROTOCOL_VERSION 3
#define MAS_CAN_ITEM_CH_RANGE 4
#define MAS_CAN_ITEM_FAULT_DETAIL 5
#define MAS_CAN_ITEM_RESTART 6

/* 異常の詳細の共通の bit（§6.9） */
#define MAS_CAN_FAULT_RESTARTED 0x01u
#define MAS_CAN_FAULT_SETTINGS_CORRUPT 0x02u
#define MAS_CAN_FAULT_BUS_OFF 0x04u

typedef struct {
    mas_can_kind_t kind;
    bool response; /* false ＝ 要求、true ＝ 応答 */
    uint8_t ch;
    mas_can_op_t op;
    uint8_t result;   /* 応答だけ */
    uint8_t item;     /* 保存と初期化では範囲（0 ＝ その ch、1 ＝ 基板の全 ch） */
    uint8_t value[4]; /* big-endian のまま */
    uint8_t seq;
} mas_can_setting_t;

void mas_can_setting_encode(const mas_can_setting_t *s, mas_can_frame_t *f);

/* 設定のフレームを解読する。DLC 8 以外、ID の下位 3 bit が 0 でないものは false（D26）。
 * 項目 0 の読み出しの応答（固有 ID）はこの形ではないので、mas_can_setting_decode_uid を使う。 */
bool mas_can_setting_decode(const mas_can_frame_t *f, mas_can_setting_t *s);

/* 項目 0（確認）の応答: data 8 byte すべてが固有 ID（D22 の並び）。 */
void mas_can_setting_encode_uid(mas_can_kind_t kind, uint64_t uid, mas_can_frame_t *f);
uint64_t mas_can_setting_decode_uid(const mas_can_frame_t *f);

/* 値の欄の読み書き（big-endian） */
void mas_can_value_set_u32(uint8_t v[4], uint32_t x);
void mas_can_value_set_i32(uint8_t v[4], int32_t x);
void mas_can_value_set_f32(uint8_t v[4], float x);
uint32_t mas_can_value_u32(const uint8_t v[4]);
int32_t mas_can_value_i32(const uint8_t v[4]);
float mas_can_value_f32(const uint8_t v[4]);

#ifdef __cplusplus
}
#endif

#endif /* MAS_CAN_CORE_SETTING_H */
