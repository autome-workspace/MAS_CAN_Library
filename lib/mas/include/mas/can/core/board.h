/* 基板の種類の一覧とプロトコルの版（純粋な C。基板のファームと主制御で共有する）。 */
#ifndef MAS_CAN_CORE_BOARD_H
#define MAS_CAN_CORE_BOARD_H

#include <stdint.h>

/* 基板の種類（C-63、C-92）。番号はここに書き足して取る（台帳の一覧と同じにする） */
#define MAS_CAN_BOARD_RESERVED 0x0000u  /* 予約（雛形はこれのまま） */
#define MAS_CAN_BOARD_MD_2CH 0x0001u    /* 新しい MD の基板（STM32F042、1 ch） */
#define MAS_CAN_BOARD_CAN_SERVO 0x0002u /* CAN-Servo 基板（RP2040-Zero、サーボ 8 ＋ MD 4） */

/* 項目 1 の値: 上位 16 bit ＝ 種類、下位 16 bit ＝ 基板の版（1 から） */
#define MAS_CAN_BOARD_TYPE(kind, rev) (((uint32_t)(kind) << 16) | (uint16_t)(rev))

/* プロトコルの版（項目 3。C-71〜74）。ライブラリと一緒に上げる。基板は node が自動で返す */
#define MAS_CAN_PROTOCOL_VERSION 0x010000C1u /* 1.0.0 */

#endif /* MAS_CAN_CORE_BOARD_H */
