#include "mas/can/node.h"

#include <string.h>

#define CMD_TIMEOUT_US 100000u  /* 指令の途絶 */
#define SYNC_TIMEOUT_US 250000u /* SYNC の途絶 */
#define SAVED_MAGIC 0x4D415331u /* "MAS1" */
#define SAVED_VERSION 1u

/* ---- 小道具 ---- */

static bool elapsed(uint32_t now, uint32_t since, uint32_t period) {
    return (uint32_t)(now - since) >= period;
}

static bool finite_nonneg(float v) { return v == v && v >= 0.0f && v < 3.4e38f; }
static bool finite_pos(float v) { return finite_nonneg(v) && v > 0.0f; }

static uint32_t crc32(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void push_tx(mas_can_node_t* n, const mas_can_frame_t* f) {
    if (n->txq_len >= MAS_CAN_NODE_TXQ) return; /* 溢れたら捨てる（FB は次の周期で送り直す） */
    n->txq[(n->txq_head + n->txq_len) % MAS_CAN_NODE_TXQ] = *f;
    n->txq_len++;
}

static int local_of(const mas_can_node_t* n, uint8_t ch) {
    if (ch < n->cfg.md_first_ch || ch >= n->cfg.md_first_ch + n->cfg.md_count) return -1;
    return ch - n->cfg.md_first_ch;
}

/* ---- 設定の焼いた値と、保存した値 ---- */

static void burned_defaults(const mas_can_node_t* n, mas_can_md_settings_t* s) {
    memset(s, 0, sizeof(*s));
    s->current_limit_ma = n->cfg.md_current_limit_default_ma;
    s->stop_brake = !n->cfg.md_has_coast;
    s->fb_rate_hz = 100;
    s->encoder_cpr = n->cfg.md_encoder_cpr_default ? n->cfg.md_encoder_cpr_default : 1;
    s->gear_ratio = 1.0f;
    s->current_lsb_ma = 1.0f;
    s->speed_lsb_rpm = 1.0f;
}

static void to_saved(const mas_can_md_settings_t* s, mas_can_node_md_saved_t* d) {
    d->reverse = s->reverse;
    d->stop_brake = s->stop_brake;
    d->encoder_reverse = s->encoder_reverse;
    d->valid = 1;
    d->current_limit_ma = s->current_limit_ma;
    d->fb_rate_hz = s->fb_rate_hz;
    d->encoder_cpr = s->encoder_cpr;
    d->gear_ratio = s->gear_ratio;
    d->speed_kp = s->speed_kp;
    d->speed_ki = s->speed_ki;
    d->speed_kd = s->speed_kd;
    d->max_speed_rpm = s->max_speed_rpm;
    d->max_accel_rpm_s = s->max_accel_rpm_s;
}

static void from_saved(const mas_can_node_md_saved_t* d, mas_can_md_settings_t* s) {
    s->reverse = d->reverse;
    s->stop_brake = d->stop_brake;
    s->encoder_reverse = d->encoder_reverse;
    s->current_limit_ma = d->current_limit_ma;
    s->fb_rate_hz = d->fb_rate_hz;
    s->encoder_cpr = d->encoder_cpr;
    s->gear_ratio = d->gear_ratio;
    s->speed_kp = d->speed_kp;
    s->speed_ki = d->speed_ki;
    s->speed_kd = d->speed_kd;
    s->max_speed_rpm = d->max_speed_rpm;
    s->max_accel_rpm_s = d->max_accel_rpm_s;
}

static bool write_saved(mas_can_node_t* n) {
    n->saved.magic = SAVED_MAGIC;
    n->saved.version = SAVED_VERSION;
    n->saved.crc = crc32(&n->saved, offsetof(mas_can_node_saved_t, crc));
    return n->cfg.save && n->cfg.save(n->cfg.ctx, &n->saved, sizeof(n->saved));
}

/* ---- 共通 ---- */

void mas_can_node_init(mas_can_node_t* n, const mas_can_node_config_t* cfg, uint32_t now_us) {
    memset(n, 0, sizeof(*n));
    n->cfg = *cfg;
    if (n->cfg.md_count > MAS_CAN_NODE_MD_MAX) n->cfg.md_count = MAS_CAN_NODE_MD_MAX;
    n->fault = MAS_CAN_FAULT_RESTARTED; /* A6 */

    bool loaded = cfg->load && cfg->load(cfg->ctx, &n->saved, sizeof(n->saved));
    bool valid = loaded && n->saved.magic == SAVED_MAGIC && n->saved.version == SAVED_VERSION &&
                 n->saved.crc == crc32(&n->saved, offsetof(mas_can_node_saved_t, crc));
    if (loaded && n->saved.magic == SAVED_MAGIC && !valid)
        n->fault |= MAS_CAN_FAULT_SETTINGS_CORRUPT;
    if (!valid) memset(&n->saved, 0, sizeof(n->saved));

    for (uint8_t i = 0; i < n->cfg.md_count; i++) {
        mas_can_node_md_t* m = &n->md[i];
        burned_defaults(n, &m->set);
        if (n->saved.md[i].valid) from_saved(&n->saved.md[i], &m->set);
        m->last_fb_us = now_us;
    }
}

void mas_can_node_ready(mas_can_node_t* n, uint32_t now_us) {
    mas_can_frame_t f;
    if (n->cfg.md_count > 0) {
        mas_can_boot_encode(MAS_CAN_KIND_MD, &f);
        push_tx(n, &f);
    }
    n->ready = true;
    for (uint8_t i = 0; i < n->cfg.md_count; i++) n->md[i].last_fb_us = now_us;
}

void mas_can_node_raise_fault(mas_can_node_t* n, uint32_t bits) { n->fault |= bits; }

static void stop_all(mas_can_node_t* n) {
    for (uint8_t i = 0; i < n->cfg.md_count; i++) {
        n->md[i].commanded = false;
        n->md[i].unsupported = false;
    }
}

/* ---- 指令 ---- */

static void handle_md_command(mas_can_node_t* n, const mas_can_frame_t* f, uint32_t now) {
    mas_can_md_cmd_t cmd;
    uint8_t frame_no;
    if (!mas_can_md_cmd_parse_id(f->id, &cmd, &frame_no) || n->estop) return;
    for (uint8_t i = 0; i < n->cfg.md_count; i++) {
        uint8_t ch = (uint8_t)(n->cfg.md_first_ch + i);
        int16_t raw;
        if (ch / MAS_CAN_MD_SLOTS != frame_no || !mas_can_md_cmd_get(f, ch, &raw))
            continue; /* D9 */
        mas_can_node_md_t* m = &n->md[i];

        if (!m->commanded || m->mode != (uint8_t)cmd) m->reset_pending = true; /* D21、D23 */
        m->mode = (uint8_t)cmd;
        m->commanded = true;
        m->last_cmd_us = now;

        bool can = true;
        if (cmd == MAS_CAN_MD_CMD_CURRENT) can = n->cfg.md_has_current;
        if (cmd == MAS_CAN_MD_CMD_SPEED) can = n->cfg.md_has_speed;
        if (cmd == MAS_CAN_MD_CMD_POSITION) can = false; /* D20: 最初のファームでは実装しない */
        m->unsupported = !can;                           /* D10 */

        float sign = m->set.reverse ? -1.0f : 1.0f; /* 32: 駆動と FB の符号を両方反転 */
        m->out.brake = false;
        switch (cmd) {
            case MAS_CAN_MD_CMD_CURRENT:
                m->out.mode = MAS_CAN_MD_OUT_CURRENT;
                m->out.value = sign * raw * m->set.current_lsb_ma;
                break;
            case MAS_CAN_MD_CMD_SPEED:
                m->out.mode = MAS_CAN_MD_OUT_SPEED;
                m->out.value = sign * raw * m->set.speed_lsb_rpm;
                break;
            case MAS_CAN_MD_CMD_DUTY:
                m->out.mode = MAS_CAN_MD_OUT_DUTY;
                m->out.value = sign * mas_can_md_duty_value(raw) / (float)MAS_CAN_MD_DUTY_FULL;
                m->out.brake = mas_can_md_duty_brake(raw) || !n->cfg.md_has_coast;
                break;
            default:
                m->out.mode = MAS_CAN_MD_OUT_STOP;
                break;
        }
    }
}

/* ---- 設定 ---- */

enum { ACC_RO, ACC_RW, ACC_CMD };

static void respond(mas_can_node_t* n, const mas_can_setting_t* req, uint8_t result,
                    const uint8_t value[4]) {
    mas_can_setting_t s = *req;
    mas_can_frame_t f;
    s.response = true;
    s.result = result;
    if (value)
        memcpy(s.value, value, 4);
    else
        memset(s.value, 0, 4);
    mas_can_setting_encode(&s, &f);
    push_tx(n, &f);
}

/* 項目の今の値を v に入れる。項目が無ければ false。acc に読み書きの性質を返す。 */
static bool item_get(const mas_can_node_t* n, int local, uint8_t item, uint8_t v[4], int* acc) {
    const mas_can_md_settings_t* s = &n->md[local].set;
    *acc = ACC_RW;
    memset(v, 0, 4);
    switch (item) {
        case MAS_CAN_ITEM_BOARD_TYPE:
            *acc = ACC_RO;
            mas_can_value_set_u32(v, n->cfg.board_type);
            return true;
        case MAS_CAN_ITEM_FW_VERSION:
            *acc = ACC_RO;
            mas_can_value_set_u32(v, n->cfg.fw_version);
            return true;
        case MAS_CAN_ITEM_PROTOCOL_VERSION:
            *acc = ACC_RO;
            mas_can_value_set_u32(v, n->cfg.protocol_version);
            return true;
        case MAS_CAN_ITEM_CH_RANGE:
            *acc = ACC_RO;
            mas_can_put_u16(&v[0], n->cfg.md_first_ch);
            mas_can_put_u16(&v[2], n->cfg.md_count);
            return true;
        case MAS_CAN_ITEM_FAULT_DETAIL:
            mas_can_value_set_u32(v, n->fault);
            return true;
        case MAS_CAN_ITEM_RESTART:
            *acc = ACC_CMD;
            return true;
        case 32:
            mas_can_value_set_u32(v, s->reverse);
            return true;
        case 33:
            mas_can_value_set_u32(v, s->current_limit_ma);
            return true;
        case 34:
            mas_can_value_set_u32(v, s->stop_brake);
            return true;
        case 35:
            mas_can_value_set_u32(v, s->fb_rate_hz);
            return true;
        case 36:
            mas_can_value_set_u32(v, s->encoder_cpr);
            return true;
        case 37:
            mas_can_value_set_f32(v, s->gear_ratio);
            return true;
        case 38:
            mas_can_value_set_u32(v, s->encoder_reverse);
            return true;
        case 39:
            mas_can_value_set_f32(v, s->speed_kp);
            return true;
        case 40:
            mas_can_value_set_f32(v, s->speed_ki);
            return true;
        case 41:
            mas_can_value_set_f32(v, s->speed_kd);
            return true;
        case 45:
            mas_can_value_set_f32(v, s->max_speed_rpm);
            return true;
        case 46:
            mas_can_value_set_f32(v, s->max_accel_rpm_s);
            return true;
        case 48:
            mas_can_value_set_f32(v, s->current_lsb_ma);
            return true;
        case 49:
            mas_can_value_set_f32(v, s->speed_lsb_rpm);
            return true;
        default:
            return false; /* 42〜44、47 は最初のファームでは無い（D20） */
    }
}

/* 書く。範囲外なら 3、能力を超える値は抑えて受け付ける（D19）。 */
static uint8_t item_set(mas_can_node_t* n, int local, uint8_t item, const uint8_t v[4]) {
    mas_can_md_settings_t* s = &n->md[local].set;
    uint32_t u = mas_can_value_u32(v);
    float x = mas_can_value_f32(v);
    switch (item) {
        case MAS_CAN_ITEM_FAULT_DETAIL:
            n->fault &= ~u;
            return MAS_CAN_RES_OK;
        case 32:
            if (u > 1) return MAS_CAN_RES_OUT_OF_RANGE;
            s->reverse = u;
            return MAS_CAN_RES_OK;
        case 33:
            s->current_limit_ma =
                u > n->cfg.md_current_limit_max_ma ? n->cfg.md_current_limit_max_ma : u;
            return MAS_CAN_RES_OK;
        case 34:
            if (u > 1) return MAS_CAN_RES_OUT_OF_RANGE;
            s->stop_brake = u || !n->cfg.md_has_coast;
            return MAS_CAN_RES_OK;
        case 35:
            s->fb_rate_hz = u > n->cfg.md_fb_rate_max_hz ? n->cfg.md_fb_rate_max_hz : u;
            return MAS_CAN_RES_OK;
        case 36:
            if (u == 0) return MAS_CAN_RES_OUT_OF_RANGE;
            s->encoder_cpr = u;
            return MAS_CAN_RES_OK;
        case 37:
            if (!finite_pos(x)) return MAS_CAN_RES_OUT_OF_RANGE;
            s->gear_ratio = x;
            return MAS_CAN_RES_OK;
        case 38:
            if (u > 1) return MAS_CAN_RES_OUT_OF_RANGE;
            s->encoder_reverse = u;
            return MAS_CAN_RES_OK;
        case 39:
        case 40:
        case 41:
        case 45:
        case 46: {
            if (!finite_nonneg(x)) return MAS_CAN_RES_OUT_OF_RANGE;
            float* dst = item == 39   ? &s->speed_kp
                         : item == 40 ? &s->speed_ki
                         : item == 41 ? &s->speed_kd
                         : item == 45 ? &s->max_speed_rpm
                                      : &s->max_accel_rpm_s;
            *dst = x;
            return MAS_CAN_RES_OK;
        }
        case 48:
            if (!finite_pos(x)) return MAS_CAN_RES_OUT_OF_RANGE;
            s->current_lsb_ma = x;
            return MAS_CAN_RES_OK;
        case 49:
            if (!finite_pos(x)) return MAS_CAN_RES_OUT_OF_RANGE;
            s->speed_lsb_rpm = x;
            return MAS_CAN_RES_OK;
        default:
            return MAS_CAN_RES_NOT_ALLOWED;
    }
}

static void handle_setting(mas_can_node_t* n, const mas_can_frame_t* f) {
    mas_can_setting_t req;
    if (!mas_can_setting_decode(f, &req) || req.response || req.kind != MAS_CAN_KIND_MD) return;
    int local = local_of(n, req.ch);
    if (local < 0) return; /* 受け持たない ch には何も返さない */

    uint8_t v[4];
    int acc;
    if (req.op == MAS_CAN_OP_READ || req.op == MAS_CAN_OP_WRITE) {
        if (req.op == MAS_CAN_OP_READ && req.item == MAS_CAN_ITEM_CHECK) {
            mas_can_frame_t out;
            mas_can_setting_encode_uid(MAS_CAN_KIND_MD, n->cfg.uid, &out);
            push_tx(n, &out);
            return;
        }
        if (req.item == MAS_CAN_ITEM_CHECK)
            acc = ACC_RO, memset(v, 0, 4);
        else if (!item_get(n, local, req.item, v, &acc)) {
            respond(n, &req, MAS_CAN_RES_UNKNOWN_ITEM, NULL);
            return;
        }
        if (req.op == MAS_CAN_OP_READ) {
            respond(n, &req, acc == ACC_CMD ? MAS_CAN_RES_NOT_ALLOWED : MAS_CAN_RES_OK,
                    acc == ACC_CMD ? NULL : v);
            return;
        }
        if (acc == ACC_RO) {
            respond(n, &req, MAS_CAN_RES_NOT_ALLOWED, v);
            return;
        }
        if (req.item == MAS_CAN_ITEM_RESTART) { /* D28 */
            bool sig = req.value[0] == 'r' && req.value[1] == 's' && req.value[2] == 't';
            respond(n, &req, sig ? MAS_CAN_RES_OK : MAS_CAN_RES_OUT_OF_RANGE,
                    sig ? req.value : NULL);
            if (sig) n->restart_pending = true;
            return;
        }
        uint8_t res = item_set(n, local, req.item, req.value);
        item_get(n, local, req.item, v, &acc);
        respond(n, &req, res, v); /* 反映後の値、失敗なら変わっていない値 */
        return;
    }

    /* 保存と初期化（範囲は byte 2: 0 ＝ その ch、1 ＝ 基板の全 ch。D29） */
    if (req.item > 1) {
        respond(n, &req, MAS_CAN_RES_OUT_OF_RANGE, NULL);
        return;
    }
    for (uint8_t i = 0; i < n->cfg.md_count; i++) {
        if (req.item == 0 && i != local) continue;
        if (req.op == MAS_CAN_OP_SAVE) {
            to_saved(&n->md[i].set, &n->saved.md[i]);
        } else { /* 初期化: 保存できる項目だけ焼いた値に戻す。48、49 は触らない */
            mas_can_md_settings_t def;
            burned_defaults(n, &def);
            def.current_lsb_ma = n->md[i].set.current_lsb_ma;
            def.speed_lsb_rpm = n->md[i].set.speed_lsb_rpm;
            n->md[i].set = def;
            memset(&n->saved.md[i], 0, sizeof(n->saved.md[i]));
        }
    }
    respond(n, &req, write_saved(n) ? MAS_CAN_RES_OK : MAS_CAN_RES_OTHER, NULL);
}

void mas_can_node_on_frame(mas_can_node_t* n, const mas_can_frame_t* f, uint32_t now_us) {
    uint32_t t;
    if (mas_can_is_estop(f)) {
        n->estop = true;
        stop_all(n);
    } else if (mas_can_is_estop_clear(f)) {
        if (n->estop) {
            n->estop = false; /* D21: 起動直後と同じ状態から（位置は続ける） */
            stop_all(n);
        }
    } else if (mas_can_sync_decode(f, &t)) {
        n->synced = true;
        n->last_sync_us = now_us;
        n->sync_lost = false;
    } else if (f->id >= 0x210u && f->id <= 0x22Fu) {
        handle_md_command(n, f, now_us);
    } else if (f->id == mas_can_setting_id(MAS_CAN_KIND_MD, 0)) {
        handle_setting(n, f);
    }
}

/* ---- 周期の処理 ---- */

static bool md_stopped(const mas_can_node_t* n, const mas_can_node_md_t* m) {
    return n->estop || n->sync_lost || !m->commanded || m->unsupported ||
           m->out.mode == MAS_CAN_MD_OUT_STOP || m->meas.protecting;
}

static void send_fb(mas_can_node_t* n, uint8_t i) {
    const mas_can_node_md_t* m = &n->md[i];
    float sign = m->set.reverse ? -1.0f : 1.0f;
    mas_can_md_fb_t fb = {0};
    fb.status = (uint8_t)(m->mode << MAS_CAN_MD_ST_MODE_SHIFT);
    if (n->fault) fb.status |= MAS_CAN_MD_ST_FAULT; /* D15 */
    if (!n->cfg.md_has_speed) fb.status |= MAS_CAN_MD_ST_SPEED_INVALID;
    if (!n->cfg.md_has_current) fb.status |= MAS_CAN_MD_ST_CURRENT_INVALID;
    if (m->meas.current_limited) fb.status |= MAS_CAN_MD_ST_CURRENT_LIMIT;
    if (md_stopped(n, m)) fb.status |= MAS_CAN_MD_ST_STOPPED;
    if (n->estop) fb.status |= MAS_CAN_MD_ST_ESTOP;
    fb.current = mas_can_sat16(sign * m->meas.current_ma / m->set.current_lsb_ma);
    fb.speed = mas_can_sat16(sign * m->meas.speed_rpm / m->set.speed_lsb_rpm);
    fb.position =
        (m->set.reverse ? (uint32_t)(0u - m->meas.position) : m->meas.position) & 0xFFFFFFu;
    fb.duty =
        mas_can_md_duty_pack(mas_can_sat16(sign * m->meas.duty * MAS_CAN_MD_DUTY_FULL), false);
    mas_can_frame_t f;
    mas_can_md_fb_encode(&fb, (uint8_t)(n->cfg.md_first_ch + i), &f);
    push_tx(n, &f);
}

void mas_can_node_tick(mas_can_node_t* n, uint32_t now_us) {
    if (n->synced && !n->sync_lost && elapsed(now_us, n->last_sync_us, SYNC_TIMEOUT_US)) {
        n->sync_lost = true; /* ラッチしない。次の指令から動く */
        stop_all(n);
    }
    for (uint8_t i = 0; i < n->cfg.md_count; i++) {
        mas_can_node_md_t* m = &n->md[i];
        if (m->commanded && elapsed(now_us, m->last_cmd_us, CMD_TIMEOUT_US)) {
            m->commanded = false;
            m->unsupported = false;
        }
        uint32_t rate = m->set.fb_rate_hz;
        if (n->ready && rate > 0 && elapsed(now_us, m->last_fb_us, 1000000u / rate)) {
            m->last_fb_us = now_us;
            send_fb(n, i);
        }
    }
}

const mas_can_frame_t* mas_can_node_peek_tx(const mas_can_node_t* n) {
    return n->txq_len ? &n->txq[n->txq_head] : NULL;
}

void mas_can_node_drop_tx(mas_can_node_t* n) {
    if (!n->txq_len) return;
    n->txq_head = (uint8_t)((n->txq_head + 1) % MAS_CAN_NODE_TXQ);
    n->txq_len--;
    if (n->txq_len == 0 && n->restart_pending && n->cfg.restart) {
        n->restart_pending = false;
        n->cfg.restart(n->cfg.ctx);
    }
}

/* ---- MD ---- */

mas_can_md_output_t mas_can_md_output(mas_can_node_t* n, uint8_t local) {
    mas_can_md_output_t out = {MAS_CAN_MD_OUT_STOP, true, 0.0f, false};
    if (local >= n->cfg.md_count) return out;
    mas_can_node_md_t* m = &n->md[local];
    if (n->estop) {
        out.brake = !n->cfg.md_has_coast; /* E-stop は常に空転（出せなければブレーキに丸める） */
        return out;
    }
    if (md_stopped(n, m)) {
        out.brake = m->set.stop_brake;
        return out;
    }
    out = m->out;
    out.reset_ctrl = m->reset_pending;
    m->reset_pending = false;
    return out;
}

void mas_can_md_set_measure(mas_can_node_t* n, uint8_t local, const mas_can_md_measure_t* meas) {
    if (local < n->cfg.md_count) n->md[local].meas = *meas;
}

const mas_can_md_settings_t* mas_can_md_settings(const mas_can_node_t* n, uint8_t local) {
    return local < n->cfg.md_count ? &n->md[local].set : NULL;
}
