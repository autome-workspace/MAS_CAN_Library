/* MAS CAN のフレームの型と ID の計算（純粋な C）。
 *
 * ハードウェアに依存しない。主制御のライブラリと、基板のファームウェアの両方が使う。
 * 仕様は docs/redesign/spec-draft.md。
 */
#ifndef MAS_CAN_CORE_FRAME_H
#define MAS_CAN_CORE_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CAN のフレーム 1 本（標準 ID、データフレーム）。 */
typedef struct {
    uint16_t id; /* 11 bit */
    uint8_t dlc; /* 0〜8 */
    bool rtr;    /* リモートフレームか（D35: 0x000 以外は無視する） */
    uint8_t data[8];
} mas_can_frame_t;

/* ---- class 0 ---- */
#define MAS_CAN_ID_ESTOP 0x000u
#define MAS_CAN_ID_ESTOP_CLEAR 0x001u
#define MAS_CAN_ID_SYNC 0x002u
#define MAS_CAN_ID_BOOT_BASE 0x010u /* + 種類の番号 */

/* ---- 種類の番号（class 0 の起動の知らせと class 4 の設定で使う） ---- */
typedef enum {
    MAS_CAN_KIND_MD = 0,
    MAS_CAN_KIND_SERVO = 1,
    MAS_CAN_KIND_OUTPUT = 2,
    MAS_CAN_KIND_INPUT = 3,
    MAS_CAN_KIND_TOF_MATRIX = 4,
    MAS_CAN_KIND_TOF_SINGLE = 5,
    MAS_CAN_KIND_CONTROLLER = 6,
    MAS_CAN_KIND_NEOPIXEL = 7,
    MAS_CAN_KIND_ENCODER = 8,
} mas_can_kind_t;

/* ---- MD の指令（class 2）の番号 ---- */
typedef enum {
    MAS_CAN_MD_CMD_CURRENT = 0,
    MAS_CAN_MD_CMD_SPEED = 1,
    MAS_CAN_MD_CMD_DUTY = 2,
    MAS_CAN_MD_CMD_POSITION = 3,
} mas_can_md_cmd_t;

#define MAS_CAN_MD_MAX_CH 32u
#define MAS_CAN_MD_SLOTS 4u /* 1 フレームの ch 数 */
#define MAS_CAN_CONTROLLER_MAX_CH 8u

/* MD の指令の ID（ch が入るフレーム）。 */
static inline uint16_t mas_can_md_cmd_id(mas_can_md_cmd_t cmd, uint8_t ch) {
    return (uint16_t)(0x210u + (unsigned)cmd * 8u + ch / MAS_CAN_MD_SLOTS);
}

/* MD の FB の ID。 */
static inline uint16_t mas_can_md_fb_id(uint8_t ch) { return (uint16_t)(0x320u + ch); }

/* コントローラの状態の ID。 */
static inline uint16_t mas_can_controller_id(uint8_t ch) { return (uint16_t)(0x340u + ch); }

/* 設定のフレームの ID（dir: 0 ＝ 要求、1 ＝ 応答）。 */
static inline uint16_t mas_can_setting_id(mas_can_kind_t kind, unsigned dir) {
    return (uint16_t)(0x400u + (unsigned)kind * 16u + dir * 8u);
}

/* ---- big-endian の詰め書き（D6） ---- */
static inline void mas_can_put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline uint16_t mas_can_get_u16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline void mas_can_put_u24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}
static inline uint32_t mas_can_get_u24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static inline void mas_can_put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline uint32_t mas_can_get_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#ifdef __cplusplus
}
#endif

#endif /* MAS_CAN_CORE_FRAME_H */
