#include "mas/can/core/setting.h"

#include <string.h>

bool mas_can_is_estop(const mas_can_frame_t* f) { return f->id == MAS_CAN_ID_ESTOP; }

bool mas_can_is_estop_clear(const mas_can_frame_t* f) {
    return f->id == MAS_CAN_ID_ESTOP_CLEAR && !f->rtr && f->dlc == 3 && f->data[0] == 'c' &&
           f->data[1] == 'l' && f->data[2] == 'r';
}

void mas_can_estop_clear_encode(mas_can_frame_t* f) {
    f->id = MAS_CAN_ID_ESTOP_CLEAR;
    f->rtr = false;
    f->dlc = 3;
    f->data[0] = 'c';
    f->data[1] = 'l';
    f->data[2] = 'r';
}

void mas_can_sync_encode(uint32_t time_us, mas_can_frame_t* f) {
    f->id = MAS_CAN_ID_SYNC;
    f->rtr = false;
    f->dlc = 4;
    mas_can_put_u32(f->data, time_us);
}

bool mas_can_sync_decode(const mas_can_frame_t* f, uint32_t* time_us) {
    if (f->id != MAS_CAN_ID_SYNC || f->rtr || f->dlc != 4) return false;
    *time_us = mas_can_get_u32(f->data);
    return true;
}

void mas_can_boot_encode(mas_can_kind_t kind, mas_can_frame_t* f) {
    f->id = (uint16_t)(MAS_CAN_ID_BOOT_BASE + (unsigned)kind);
    f->rtr = false;
    f->dlc = 0;
}

bool mas_can_boot_decode(const mas_can_frame_t* f, mas_can_kind_t* kind) {
    if (f->id < MAS_CAN_ID_BOOT_BASE || f->id > MAS_CAN_ID_BOOT_BASE + 15u || f->rtr || f->dlc != 0)
        return false;
    *kind = (mas_can_kind_t)(f->id - MAS_CAN_ID_BOOT_BASE);
    return true;
}

void mas_can_setting_encode(const mas_can_setting_t* s, mas_can_frame_t* f) {
    f->id = mas_can_setting_id(s->kind, s->response ? 1u : 0u);
    f->rtr = false;
    f->dlc = 8;
    f->data[0] = s->ch;
    f->data[1] = (uint8_t)(((unsigned)s->op << 6) | (s->response ? (s->result & 0x3Fu) : 0u));
    f->data[2] = s->item;
    memcpy(&f->data[3], s->value, 4);
    f->data[7] = s->seq;
}

bool mas_can_setting_decode(const mas_can_frame_t* f, mas_can_setting_t* s) {
    if (f->id < 0x400u || f->id > 0x4FFu || (f->id & 7u) != 0 || f->rtr || f->dlc != 8)
        return false;
    s->kind = (mas_can_kind_t)((f->id - 0x400u) >> 4);
    s->response = ((f->id >> 3) & 1u) != 0;
    s->ch = f->data[0];
    s->op = (mas_can_op_t)(f->data[1] >> 6);
    s->result = (uint8_t)(f->data[1] & 0x3Fu);
    s->item = f->data[2];
    memcpy(s->value, &f->data[3], 4);
    s->seq = f->data[7];
    return true;
}

void mas_can_setting_encode_uid(mas_can_kind_t kind, uint64_t uid, mas_can_frame_t* f) {
    f->id = mas_can_setting_id(kind, 1);
    f->rtr = false;
    f->dlc = 8;
    mas_can_put_u32(&f->data[0], (uint32_t)(uid >> 32));
    mas_can_put_u32(&f->data[4], (uint32_t)uid);
}

uint64_t mas_can_setting_decode_uid(const mas_can_frame_t* f) {
    return ((uint64_t)mas_can_get_u32(&f->data[0]) << 32) | mas_can_get_u32(&f->data[4]);
}

void mas_can_value_set_u32(uint8_t v[4], uint32_t x) { mas_can_put_u32(v, x); }
void mas_can_value_set_i32(uint8_t v[4], int32_t x) { mas_can_put_u32(v, (uint32_t)x); }
void mas_can_value_set_f32(uint8_t v[4], float x) {
    uint32_t u;
    memcpy(&u, &x, 4);
    mas_can_put_u32(v, u);
}
uint32_t mas_can_value_u32(const uint8_t v[4]) { return mas_can_get_u32(v); }
int32_t mas_can_value_i32(const uint8_t v[4]) { return (int32_t)mas_can_get_u32(v); }
float mas_can_value_f32(const uint8_t v[4]) {
    uint32_t u = mas_can_get_u32(v);
    float x;
    memcpy(&x, &u, 4);
    return x;
}
