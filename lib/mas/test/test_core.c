// コア（純粋な C）のテスト。PC で動かす: make -C lib/mas/test
#include <stdio.h>
#include <string.h>

#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static void test_ids(void) {
    CHECK(mas_can_md_cmd_id(MAS_CAN_MD_CMD_CURRENT, 0) == 0x210);
    CHECK(mas_can_md_cmd_id(MAS_CAN_MD_CMD_SPEED, 5) == 0x219);
    CHECK(mas_can_md_cmd_id(MAS_CAN_MD_CMD_POSITION, 31) == 0x22F);
    CHECK(mas_can_md_fb_id(31) == 0x33F);
    CHECK(mas_can_setting_id(MAS_CAN_KIND_MD, 1) == 0x408);
    CHECK(mas_can_setting_id(MAS_CAN_KIND_SERVO, 0) == 0x410);

    mas_can_md_cmd_t cmd;
    uint8_t fr;
    CHECK(mas_can_md_cmd_parse_id(0x221, &cmd, &fr) && cmd == MAS_CAN_MD_CMD_DUTY && fr == 1);
    CHECK(!mas_can_md_cmd_parse_id(0x230, &cmd, &fr));
}

static void test_cmd_slots(void) {
    mas_can_frame_t f;
    mas_can_md_cmd_init(&f, MAS_CAN_MD_CMD_POSITION, 6);
    CHECK(f.id == 0x229 && f.dlc == 8);
    int16_t v;
    CHECK(mas_can_md_cmd_get(&f, 4, &v) && v == MAS_CAN_MD_POSITION_STOP);

    mas_can_md_cmd_init(&f, MAS_CAN_MD_CMD_SPEED, 5);
    mas_can_md_cmd_set(&f, 5, -1234);
    CHECK(f.data[2] == 0xFB && f.data[3] == 0x2E); // -1234 = 0xFB2E（BE）
    CHECK(mas_can_md_cmd_get(&f, 5, &v) && v == -1234);

    // D9: DLC の外の slot は受けていない扱い。奇数の DLC は欠けた slot だけ
    bool used[4] = {true, true, false, false};
    mas_can_md_cmd_trim(&f, used);
    CHECK(f.dlc == 4);
    CHECK(mas_can_md_cmd_get(&f, 5, &v));
    CHECK(!mas_can_md_cmd_get(&f, 6, &v));
    f.dlc = 3;
    CHECK(mas_can_md_cmd_get(&f, 4, &v));
    CHECK(!mas_can_md_cmd_get(&f, 5, &v));
}

static void test_duty(void) {
    CHECK(mas_can_md_duty_pack(0, false) == 0x0000);
    CHECK(mas_can_md_duty_pack(0, true) == 0x0001);
    CHECK(mas_can_md_duty_pack(20000, false) == 32766);
    CHECK(mas_can_md_duty_value(mas_can_md_duty_pack(-16383, true)) == -16383);
    CHECK(mas_can_md_duty_brake(mas_can_md_duty_pack(-100, true)));
    CHECK(mas_can_md_duty_value(mas_can_md_duty_pack(-100, true)) == -100);
    CHECK(mas_can_md_duty_value((int16_t)0x8000) == -16383); // Q1: -16384 は -100% に丸める
    CHECK(mas_can_md_duty_value((int16_t)0x8001) == -16383);
}

static void test_fb(void) {
    mas_can_frame_t f;
    mas_can_md_fb_t in = {0}, out;
    uint8_t ch;

    in.status = 0x40; // 速度の指令に従っている
    in.current = 1500;
    in.speed = -300;
    in.position = 0x123456;
    mas_can_md_fb_encode(&in, 7, &f);
    CHECK(f.id == 0x327 && f.dlc == 8);
    CHECK(mas_can_md_fb_decode(&f, &ch, &out) && ch == 7);
    CHECK(out.current == 1500 && out.speed == -300 && out.position == 0x123456);

    in.status = MAS_CAN_MD_ST_SPEED_INVALID; // 電流だけ: DLC 5、byte 3–4 は duty
    in.duty = mas_can_md_duty_pack(8000, true);
    mas_can_md_fb_encode(&in, 0, &f);
    CHECK(f.dlc == 5);
    CHECK(mas_can_md_fb_decode(&f, &ch, &out) && out.current == 1500 && out.duty == in.duty);

    in.status = MAS_CAN_MD_ST_SPEED_INVALID | MAS_CAN_MD_ST_CURRENT_INVALID; // DLC 3
    mas_can_md_fb_encode(&in, 0, &f);
    CHECK(f.dlc == 3);
    CHECK(mas_can_md_fb_decode(&f, &ch, &out) && out.duty == in.duty && out.current == 0);

    in.status = MAS_CAN_MD_ST_CURRENT_INVALID; // エンコーダだけ: byte 1–2 が duty
    mas_can_md_fb_encode(&in, 0, &f);
    CHECK(f.dlc == 8);
    CHECK(mas_can_md_fb_decode(&f, &ch, &out) && out.duty == in.duty && out.speed == -300);

    f.dlc = 5; // 状態の bit と DLC が合わない
    CHECK(!mas_can_md_fb_decode(&f, &ch, &out));
}

static void test_helpers(void) {
    CHECK(mas_can_sat16(40000.0f) == 32767);
    CHECK(mas_can_sat16(-40000.0f) == -32767); // D17: -32768 は使わない
    CHECK(mas_can_sat16(1.5f) == 2 && mas_can_sat16(-1.5f) == -2);
    float nan = 0.0f / 0.0f;
    CHECK(mas_can_sat16(nan) == 0);
    CHECK(mas_can_u24_diff(5, 0xFFFFFE) == 7);
    CHECK(mas_can_u24_diff(0xFFFFFE, 5) == -7);
}

static void test_class0(void) {
    mas_can_frame_t f = {0};
    f.id = 0x000;
    f.rtr = true;
    CHECK(mas_can_is_estop(&f)); // D35: リモートフレームでも止める

    mas_can_estop_clear_encode(&f);
    CHECK(mas_can_is_estop_clear(&f));
    f.dlc = 8;
    CHECK(!mas_can_is_estop_clear(&f)); // D25: DLC 3 ちょうど

    uint32_t t;
    mas_can_sync_encode(0x01020304, &f);
    CHECK(f.data[0] == 1 && f.data[3] == 4);
    CHECK(mas_can_sync_decode(&f, &t) && t == 0x01020304);
    f.dlc = 5;
    CHECK(!mas_can_sync_decode(&f, &t)); // D24

    mas_can_kind_t k;
    mas_can_boot_encode(MAS_CAN_KIND_MD, &f);
    CHECK(f.id == 0x010 && f.dlc == 0);
    CHECK(mas_can_boot_decode(&f, &k) && k == MAS_CAN_KIND_MD);
}

static void test_setting(void) {
    mas_can_setting_t s = {0}, r;
    mas_can_frame_t f;
    s.kind = MAS_CAN_KIND_MD;
    s.response = true;
    s.ch = 3;
    s.op = MAS_CAN_OP_WRITE;
    s.result = MAS_CAN_RES_OUT_OF_RANGE;
    s.item = 49;
    mas_can_value_set_f32(s.value, 1.0f);
    s.seq = 42;
    mas_can_setting_encode(&s, &f);
    CHECK(f.id == 0x408 && f.dlc == 8 && f.data[1] == 0x43);
    CHECK(f.data[3] == 0x3F && f.data[4] == 0x80 && f.data[5] == 0 && f.data[6] == 0);
    CHECK(mas_can_setting_decode(&f, &r));
    CHECK(r.response && r.ch == 3 && r.op == MAS_CAN_OP_WRITE && r.result == 3 && r.item == 49);
    CHECK(mas_can_value_f32(r.value) == 1.0f && r.seq == 42);

    s.response = false; // 要求では結果の欄は 0
    mas_can_setting_encode(&s, &f);
    CHECK(f.id == 0x400 && f.data[1] == 0x40);

    f.dlc = 7;
    CHECK(!mas_can_setting_decode(&f, &r)); // D26
    f.dlc = 8;
    f.id = 0x401;
    CHECK(!mas_can_setting_decode(&f, &r));

    mas_can_setting_encode_uid(MAS_CAN_KIND_MD, 0x0102030405060708ull, &f);
    CHECK(f.id == 0x408 && f.data[0] == 1 && f.data[7] == 8);
    CHECK(mas_can_setting_decode_uid(&f) == 0x0102030405060708ull);
}

int main(void) {
    test_ids();
    test_cmd_slots();
    test_duty();
    test_fb();
    test_helpers();
    test_class0();
    test_setting();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
