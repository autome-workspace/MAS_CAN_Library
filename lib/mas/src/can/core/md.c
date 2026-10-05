#include "mas/can/core/md.h"

int16_t mas_can_md_stop_value(mas_can_md_cmd_t cmd) {
    return cmd == MAS_CAN_MD_CMD_POSITION ? MAS_CAN_MD_POSITION_STOP : 0;
}

void mas_can_md_cmd_init(mas_can_frame_t *f, mas_can_md_cmd_t cmd, uint8_t ch) {
    f->id = mas_can_md_cmd_id(cmd, ch);
    f->dlc = 8;
    f->rtr = false;
    uint16_t stop = (uint16_t)mas_can_md_stop_value(cmd);
    for (unsigned i = 0; i < MAS_CAN_MD_SLOTS; i++) mas_can_put_u16(&f->data[i * 2], stop);
}

void mas_can_md_cmd_set(mas_can_frame_t *f, uint8_t ch, int16_t value) {
    mas_can_put_u16(&f->data[(ch % MAS_CAN_MD_SLOTS) * 2], (uint16_t)value);
}

void mas_can_md_cmd_trim(mas_can_frame_t *f, const bool used[MAS_CAN_MD_SLOTS]) {
    uint8_t dlc = 0;
    for (unsigned i = 0; i < MAS_CAN_MD_SLOTS; i++) {
        if (used[i]) dlc = (uint8_t)((i + 1) * 2);
    }
    f->dlc = dlc;
}

bool mas_can_md_cmd_parse_id(uint16_t id, mas_can_md_cmd_t *cmd, uint8_t *frame_no) {
    if (id < 0x210u || id > 0x22Fu) return false;
    *cmd = (mas_can_md_cmd_t)((id - 0x210u) / 8u);
    *frame_no = (uint8_t)((id - 0x210u) % 8u);
    return true;
}

bool mas_can_md_cmd_get(const mas_can_frame_t *f, uint8_t ch, int16_t *value) {
    unsigned off = (ch % MAS_CAN_MD_SLOTS) * 2u;
    if (f->rtr || f->dlc < off + 2u) return false;
    *value = (int16_t)mas_can_get_u16(&f->data[off]);
    return true;
}

int16_t mas_can_md_duty_pack(int16_t duty, bool brake) {
    if (duty > MAS_CAN_MD_DUTY_FULL) duty = MAS_CAN_MD_DUTY_FULL;
    if (duty < -MAS_CAN_MD_DUTY_FULL) duty = -MAS_CAN_MD_DUTY_FULL;
    return (int16_t)(duty * 2 + (brake ? 1 : 0));
}

int16_t mas_can_md_duty_value(int16_t raw) {
    int32_t v = ((int32_t)raw - (int32_t)((uint16_t)raw & 1u)) / 2;
    return (int16_t)(v < -MAS_CAN_MD_DUTY_FULL ? -MAS_CAN_MD_DUTY_FULL : v);
}

bool mas_can_md_duty_brake(int16_t raw) { return ((uint16_t)raw & 1u) != 0; }

void mas_can_md_fb_encode(const mas_can_md_fb_t *fb, uint8_t ch, mas_can_frame_t *f) {
    bool speed_ok = !(fb->status & MAS_CAN_MD_ST_SPEED_INVALID);
    bool current_ok = !(fb->status & MAS_CAN_MD_ST_CURRENT_INVALID);
    f->id = mas_can_md_fb_id(ch);
    f->rtr = false;
    f->data[0] = fb->status;
    mas_can_put_u16(&f->data[1], (uint16_t)(current_ok ? fb->current : fb->duty));
    if (speed_ok) {
        mas_can_put_u16(&f->data[3], (uint16_t)fb->speed);
        mas_can_put_u24(&f->data[5], fb->position & 0xFFFFFFu);
        f->dlc = 8;
    } else if (current_ok) {
        mas_can_put_u16(&f->data[3], (uint16_t)fb->duty);
        f->dlc = 5;
    } else {
        f->dlc = 3;
    }
}

bool mas_can_md_fb_decode(const mas_can_frame_t *f, uint8_t *ch, mas_can_md_fb_t *fb) {
    if (f->rtr || f->id < 0x320u || f->id > 0x33Fu || f->dlc < 1) return false;
    uint8_t st = f->data[0];
    bool speed_ok = !(st & MAS_CAN_MD_ST_SPEED_INVALID);
    bool current_ok = !(st & MAS_CAN_MD_ST_CURRENT_INVALID);
    uint8_t want = speed_ok ? 8 : (current_ok ? 5 : 3);
    if (f->dlc != want) return false;

    *ch = (uint8_t)(f->id - 0x320u);
    fb->status = st;
    int16_t first = (int16_t)mas_can_get_u16(&f->data[1]);
    fb->current = current_ok ? first : 0;
    fb->duty = current_ok ? 0 : first;
    fb->speed = 0;
    fb->position = 0;
    if (speed_ok) {
        fb->speed = (int16_t)mas_can_get_u16(&f->data[3]);
        fb->position = mas_can_get_u24(&f->data[5]);
    } else if (current_ok) {
        fb->duty = (int16_t)mas_can_get_u16(&f->data[3]);
    }
    return true;
}

int16_t mas_can_sat16(float v) {
    if (!(v == v)) return 0; /* NaN */
    if (v >= 32767.0f) return 32767;
    if (v <= -32767.0f) return -32767;
    return (int16_t)(v >= 0 ? (int32_t)(v + 0.5f) : (int32_t)(v - 0.5f));
}

int32_t mas_can_u24_diff(uint32_t now, uint32_t prev) {
    uint32_t d = (now - prev) & 0xFFFFFFu;
    return (d & 0x800000u) ? (int32_t)d - 0x1000000 : (int32_t)d;
}
