// 基板側の処理（node）のテスト。偽のフラッシュで PC で動かす。
#include <stdio.h>
#include <string.h>

#include "mas/can/node.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static unsigned char flash[sizeof(mas_can_node_saved_t)];
static bool flash_blank = true;
static int restarts = 0;

static bool load(void* ctx, void* buf, size_t len) {
    (void)ctx;
    if (flash_blank)
        memset(buf, 0xFF, len);
    else
        memcpy(buf, flash, len);
    return true;
}
static bool save(void* ctx, const void* buf, size_t len) {
    (void)ctx;
    memcpy(flash, buf, len);
    flash_blank = false;
    return true;
}
static void restart(void* ctx) {
    (void)ctx;
    restarts++;
}

static mas_can_node_config_t config(void) {
    mas_can_node_config_t c;
    memset(&c, 0, sizeof(c));
    c.uid = 0x1122334455667788ull;
    c.board_type = 0x00010001;
    c.md_first_ch = 5;
    c.md_count = 1;
    c.md_has_current = true;
    c.md_has_speed = false;
    c.md_has_coast = false;
    c.md_current_limit_max_ma = 10000;
    c.md_current_limit_default_ma = 2400;
    c.md_fb_rate_max_hz = 1000;
    c.load = load;
    c.save = save;
    c.restart = restart;
    return c;
}

static mas_can_node_t node;
static uint32_t now = 1000;

static void tick_ms(int ms) {
    for (int i = 0; i < ms; i++) {
        now += 1000;
        mas_can_node_tick(&node, now);
    }
}

static int drain(mas_can_frame_t* out, int max) {
    int n = 0;
    const mas_can_frame_t* f;
    while ((f = mas_can_node_peek_tx(&node))) {
        if (n < max) out[n] = *f;
        n++;
        mas_can_node_drop_tx(&node);
    }
    return n;
}

static void send_cmd(mas_can_md_cmd_t cmd, int16_t v, uint8_t dlc) {
    mas_can_frame_t f;
    mas_can_md_cmd_init(&f, cmd, 5);
    mas_can_md_cmd_set(&f, 5, v);
    f.dlc = dlc;
    mas_can_node_on_frame(&node, &f, now);
}

static mas_can_setting_t setting(uint8_t ch, mas_can_op_t op, uint8_t item, uint32_t value) {
    mas_can_setting_t s;
    memset(&s, 0, sizeof(s));
    s.kind = MAS_CAN_KIND_MD;
    s.ch = ch;
    s.op = op;
    s.item = item;
    mas_can_value_set_u32(s.value, value);
    s.seq = 7;
    return s;
}

static bool request(mas_can_setting_t req, mas_can_setting_t* resp) {
    mas_can_frame_t f, out[8];
    mas_can_setting_encode(&req, &f);
    mas_can_node_on_frame(&node, &f, now);
    int n = drain(out, 8);
    for (int i = 0; i < n && i < 8; i++)
        if (mas_can_setting_decode(&out[i], resp) && resp->response) return true;
    return false;
}

static void test_boot_and_fb(void) {
    mas_can_node_config_t c = config();
    mas_can_node_init(&node, &c, now);
    mas_can_frame_t out[8];
    tick_ms(50);
    CHECK(drain(out, 8) == 0); // 起動の知らせの前は FB を送らない（D30）
    mas_can_node_ready(&node, now);
    int n = drain(out, 8);
    CHECK(n == 1 && out[0].id == 0x010 && out[0].dlc == 0);
    tick_ms(10);
    n = drain(out, 8);
    CHECK(n == 1 && out[0].id == 0x325 && out[0].dlc == 5); // 電流だけ測れる
    // 起動直後: 異常あり（再起動）、止めている、指令の種類 0（D16）
    CHECK(out[0].data[0] ==
          (MAS_CAN_MD_ST_FAULT | MAS_CAN_MD_ST_SPEED_INVALID | MAS_CAN_MD_ST_STOPPED));
    mas_can_md_output_t o = mas_can_md_output(&node, 0);
    CHECK(o.mode == MAS_CAN_MD_OUT_STOP && o.brake); // 空転を出せないのでブレーキ

    // 起動の猶予（C-89）: 知らせから 200 ms は指令に従わない
    send_cmd(MAS_CAN_MD_CMD_CURRENT, 1500, 8);
    CHECK(mas_can_md_output(&node, 0).mode == MAS_CAN_MD_OUT_STOP);
    tick_ms(200);
    drain(out, 8);
}

static void test_commands(void) {
    send_cmd(MAS_CAN_MD_CMD_CURRENT, 1500, 4);
    mas_can_md_output_t o = mas_can_md_output(&node, 0);
    CHECK(o.mode == MAS_CAN_MD_OUT_CURRENT && o.value == 1500.0f && o.reset_ctrl);
    o = mas_can_md_output(&node, 0);
    CHECK(!o.reset_ctrl); // 読んだら消える

    send_cmd(MAS_CAN_MD_CMD_CURRENT, 999, 2); // D9: ch 5 は slot 1。DLC 2 なら受けていない
    o = mas_can_md_output(&node, 0);
    CHECK(o.value == 1500.0f);

    send_cmd(MAS_CAN_MD_CMD_DUTY, mas_can_md_duty_pack(8191, false), 8);
    o = mas_can_md_output(&node, 0);
    CHECK(o.mode == MAS_CAN_MD_OUT_DUTY && o.reset_ctrl &&
          o.brake); // 種類が変わった、ブレーキに丸める
    CHECK(o.value > 0.49f && o.value < 0.51f);

    send_cmd(MAS_CAN_MD_CMD_SPEED, 100, 8); // エンコーダが無い → 止める（D10）
    o = mas_can_md_output(&node, 0);
    CHECK(o.mode == MAS_CAN_MD_OUT_STOP);
    mas_can_frame_t out[8];
    drain(out, 8);
    tick_ms(10);
    drain(out, 8);
    CHECK((out[0].data[0] & MAS_CAN_MD_ST_STOPPED) &&
          (out[0].data[0] >> 6) == MAS_CAN_MD_CMD_SPEED);

    send_cmd(MAS_CAN_MD_CMD_CURRENT, 500, 8);
    tick_ms(99);
    CHECK(mas_can_md_output(&node, 0).mode == MAS_CAN_MD_OUT_CURRENT);
    tick_ms(2); // 100 ms の途絶
    CHECK(mas_can_md_output(&node, 0).mode == MAS_CAN_MD_OUT_STOP);
}

static void test_estop_sync(void) {
    mas_can_frame_t f = {0};
    send_cmd(MAS_CAN_MD_CMD_CURRENT, 500, 8);
    f.id = 0;
    f.rtr = true; // リモートフレームでも止める（D35）
    mas_can_node_on_frame(&node, &f, now);
    CHECK(mas_can_md_output(&node, 0).mode == MAS_CAN_MD_OUT_STOP);
    send_cmd(MAS_CAN_MD_CMD_CURRENT, 500, 8); // ラッチ中は無視
    CHECK(mas_can_md_output(&node, 0).mode == MAS_CAN_MD_OUT_STOP);
    mas_can_estop_clear_encode(&f);
    mas_can_node_on_frame(&node, &f, now);
    CHECK(mas_can_md_output(&node, 0).mode == MAS_CAN_MD_OUT_STOP); // 解除だけでは動かない
    send_cmd(MAS_CAN_MD_CMD_CURRENT, 500, 8);
    mas_can_md_output_t o = mas_can_md_output(&node, 0);
    CHECK(o.mode == MAS_CAN_MD_OUT_CURRENT && o.reset_ctrl); // D21

    // E-stop は常にブレーキ（M-51）
    f.id = 0;
    f.rtr = false;
    f.dlc = 0;
    mas_can_node_on_frame(&node, &f, now);
    o = mas_can_md_output(&node, 0);
    CHECK(o.mode == MAS_CAN_MD_OUT_STOP && o.brake);
    mas_can_estop_clear_encode(&f);
    mas_can_node_on_frame(&node, &f, now);

    // SYNC が途絶えても止めない（C-90）
    mas_can_sync_encode(0, &f);
    mas_can_node_on_frame(&node, &f, now);
    for (int i = 0; i < 30; i++) { // 指令は送り続け、SYNC だけ止める
        send_cmd(MAS_CAN_MD_CMD_CURRENT, 500, 8);
        tick_ms(10);
    }
    CHECK(mas_can_md_output(&node, 0).mode == MAS_CAN_MD_OUT_CURRENT);
}

static void test_settings(void) {
    mas_can_setting_t r;
    mas_can_frame_t f, out[8];

    // 受け持たない ch には返さない
    CHECK(!request(setting(4, MAS_CAN_OP_READ, 33, 0), &r));

    // 項目 0: 固有 ID
    mas_can_setting_t q = setting(5, MAS_CAN_OP_READ, 0, 0);
    mas_can_setting_encode(&q, &f);
    mas_can_node_on_frame(&node, &f, now);
    CHECK(drain(out, 8) == 1 && mas_can_setting_decode_uid(&out[0]) == 0x1122334455667788ull);

    CHECK(request(setting(5, MAS_CAN_OP_READ, 4, 0), &r) && r.result == 0);
    CHECK(mas_can_get_u16(&r.value[0]) == 5 && mas_can_get_u16(&r.value[2]) == 1);
    CHECK(request(setting(5, MAS_CAN_OP_WRITE, 1, 0), &r) && r.result == MAS_CAN_RES_NOT_ALLOWED);
    CHECK(request(setting(5, MAS_CAN_OP_READ, 6, 0), &r) && r.result == MAS_CAN_RES_NOT_ALLOWED);
    CHECK(request(setting(5, MAS_CAN_OP_READ, 42, 0), &r) && r.result == MAS_CAN_RES_UNKNOWN_ITEM);

    // 範囲外は 3、能力を超える値は抑える（D19）
    CHECK(request(setting(5, MAS_CAN_OP_WRITE, 32, 2), &r) && r.result == MAS_CAN_RES_OUT_OF_RANGE);
    CHECK(request(setting(5, MAS_CAN_OP_WRITE, 33, 50000), &r) && r.result == 0 &&
          mas_can_value_u32(r.value) == 10000);
    CHECK(request(setting(5, MAS_CAN_OP_WRITE, 34, 0), &r) &&
          mas_can_value_u32(r.value) == 1); // 空転なし

    // 電流の分解能（保存しない）
    q = setting(5, MAS_CAN_OP_WRITE, 48, 0);
    mas_can_value_set_f32(q.value, 2.0f);
    CHECK(request(q, &r) && r.result == 0);
    send_cmd(MAS_CAN_MD_CMD_CURRENT, 500, 8);
    CHECK(mas_can_md_output(&node, 0).value == 1000.0f);

    // 異常の詳細: 再起動の bit を消す
    CHECK(request(setting(5, MAS_CAN_OP_READ, 5, 0), &r) && mas_can_value_u32(r.value) == 1);
    CHECK(request(setting(5, MAS_CAN_OP_WRITE, 5, 1), &r) && mas_can_value_u32(r.value) == 0);

    // 保存 → 作り直しても残る。初期化は保存しない項目に触らない（D29）
    CHECK(request(setting(5, MAS_CAN_OP_SAVE, 0, 0), &r) && r.result == 0);
    mas_can_node_config_t c = config();
    mas_can_node_init(&node, &c, now);
    CHECK(mas_can_md_settings(&node, 0)->current_limit_ma == 10000);
    CHECK(mas_can_md_settings(&node, 0)->current_lsb_ma == 1.0f); // 保存しない項目は焼いた値
    q = setting(5, MAS_CAN_OP_WRITE, 48, 0);
    mas_can_value_set_f32(q.value, 2.0f);
    request(q, &r);
    CHECK(request(setting(5, MAS_CAN_OP_INIT, 1, 0), &r) && r.result == 0);
    CHECK(mas_can_md_settings(&node, 0)->current_limit_ma == 2400);
    CHECK(mas_can_md_settings(&node, 0)->current_lsb_ma == 2.0f);
    CHECK(request(setting(5, MAS_CAN_OP_SAVE, 2, 0), &r) && r.result == MAS_CAN_RES_OUT_OF_RANGE);

    // 壊れた保存 → 焼いた値で起動、異常の bit 1
    flash[10] ^= 0xFF;
    mas_can_node_init(&node, &c, now);
    CHECK(node.fault == (MAS_CAN_FAULT_RESTARTED | MAS_CAN_FAULT_SETTINGS_CORRUPT));

    // 再起動: 署名が違えば 3。合えば応答を送り終えてから
    mas_can_node_ready(&node, now);
    drain(out, 8);
    CHECK(request(setting(5, MAS_CAN_OP_WRITE, 6, 0x72737400), &r) && r.result == 0 &&
          restarts == 1);
    CHECK(request(setting(5, MAS_CAN_OP_WRITE, 6, 0x12345678), &r) &&
          r.result == MAS_CAN_RES_OUT_OF_RANGE && restarts == 1);
}

int main(void) {
    test_boot_and_fb();
    test_commands();
    test_estop_sync();
    test_settings();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all node tests passed\n");
    return 0;
}
