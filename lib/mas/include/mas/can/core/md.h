/* MD の指令と FB のフレームの組み立てと解読（純粋な C）。
 *
 * 値は生の整数（LSB は基板の分解能の設定による）。SI との換算は上の層が行う。
 */
#ifndef MAS_CAN_CORE_MD_H
#define MAS_CAN_CORE_MD_H

#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 指令（class 2） ---- */

#define MAS_CAN_MD_POSITION_STOP ((int16_t)0x8000) /* 位置の止める値（D13） */

/* その指令の止める値（電流・速度・duty は 0、位置は 0x8000）。 */
int16_t mas_can_md_stop_value(mas_can_md_cmd_t cmd);

/* ch が入るフレームを、全 slot を止める値で埋めて用意する（DLC 8）。 */
void mas_can_md_cmd_init(mas_can_frame_t* f, mas_can_md_cmd_t cmd, uint8_t ch);

/* ch の slot に値を書く（フレームは mas_can_md_cmd_init で用意したもの）。 */
void mas_can_md_cmd_set(mas_can_frame_t* f, uint8_t ch, int16_t value);

/* 後ろの使っていない slot を削って DLC を縮める（任意。D9）。used は slot ごとに使っているか。 */
void mas_can_md_cmd_trim(mas_can_frame_t* f, const bool used[MAS_CAN_MD_SLOTS]);

/* 受けたフレームが MD の指令なら、指令の種類とフレーム番号を返す。 */
bool mas_can_md_cmd_parse_id(uint16_t id, mas_can_md_cmd_t* cmd, uint8_t* frame_no);

/* ch の slot を読む。slot が DLC の外（欠けを含む）なら false ＝ 受けていない扱い（D9）。 */
bool mas_can_md_cmd_get(const mas_can_frame_t* f, uint8_t ch, int16_t* value);

/* duty の slot の値（bit 0 ＝ 減衰モード、bit 1–15 ＝ duty。±16383 ＝ ±100%）。 */
#define MAS_CAN_MD_DUTY_FULL 16383
int16_t mas_can_md_duty_pack(int16_t duty, bool brake); /* duty は ±16383 に丸める */
int16_t mas_can_md_duty_value(int16_t raw);             /* -16384 は -16383 に丸める（Q1） */
bool mas_can_md_duty_brake(int16_t raw);

/* ---- FB（class 3） ---- */

/* 状態の byte の bit（spec-draft.md §7.3） */
#define MAS_CAN_MD_ST_FAULT 0x01u         /* 基板に異常あり */
#define MAS_CAN_MD_ST_SPEED_INVALID 0x02u /* 速度と位置が無効 */
#define MAS_CAN_MD_ST_CURRENT_INVALID 0x04u
#define MAS_CAN_MD_ST_CURRENT_LIMIT 0x08u
#define MAS_CAN_MD_ST_STOPPED 0x10u
#define MAS_CAN_MD_ST_ESTOP 0x20u
#define MAS_CAN_MD_ST_MODE_SHIFT 6 /* bit 6–7: 今従っている指令の種類 */

typedef struct {
    uint8_t status;
    int16_t current;   /* 電流が無効なら 0 */
    int16_t speed;     /* 速度が無効なら 0 */
    uint32_t position; /* 24 bit。速度が無効なら 0 */
    int16_t duty;      /* 実際に出している duty（slot の形）。空いた欄が無ければ送られない */
} mas_can_md_fb_t;

/* FB を組み立てる。中身の並びと DLC は状態の bit 1、bit 2 で決まる（8、5、3）。 */
void mas_can_md_fb_encode(const mas_can_md_fb_t* fb, uint8_t ch, mas_can_frame_t* f);

/* FB を解読する。ID が FB でないか、DLC が状態の bit と合わなければ false。 */
bool mas_can_md_fb_decode(const mas_can_frame_t* f, uint8_t* ch, mas_can_md_fb_t* fb);

/* ---- 換算の補助 ---- */

/* 実数を int16 にする。±32767 で飽和し、-32768 は使わない（D17）。NaN は 0。 */
int16_t mas_can_sat16(float v);

/* 24 bit で折り返すカウンタの差（now − prev）を、−2^23〜2^23−1 で返す。 */
int32_t mas_can_u24_diff(uint32_t now, uint32_t prev);

#ifdef __cplusplus
}
#endif

#endif /* MAS_CAN_CORE_MD_H */
