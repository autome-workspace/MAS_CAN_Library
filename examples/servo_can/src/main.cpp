// CAN-Servo 基板のサーボと MD を 1 つずつ動かす例。
// 基板 1 枚にサーボ 8 ch と MD 4 ch が同居している（ch の先頭は基板のファームの
// SERVO_FIRST_CH、MD_FIRST_CH）。MD は duty だけで、電流・速度は測れない。
// ブレーキは出せないので空転に丸められる。
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Servo arm = can.servo(0); // ch 0 のサーボ（J2）
mcan::Md wheel = can.md(2);     // ch 2 の MD。同じ ch の MD 基板と一緒に繋がない

void setup() {
    Serial.begin(115200);
    // RP2040 は 3 つ目の引数で PIO のブロックを選べる（既定 1）: can.begin(CAN_TX, CAN_RX, 0)
    if (!can.begin(CAN_TX, CAN_RX)) Serial.println(mcan::toString(can.error()));
    arm.configCalibration(-1.57f, 1.57f, 500, 2500); // -90° で 500 µs、+90° で 2500 µs
    if (!can.waitReady(1000)) Serial.println("MD not ready"); // 待つのは MD だけ（サーボに FB は無い）
}

void loop() {
    can.update();

    float t = millis() * 0.001f;
    arm.setAngle(sinf(t) * 1.0f);         // ±1 rad をゆっくり往復
    wheel.setDuty(1, false);              // duty 100% のまま。false で空転

    static uint32_t last = 0;
    if (millis() - last >= 500) {
        last = millis();
        Serial.printf("%s duty %+.2f\n", wheel.online() ? "online" : "offline",
                      wheel.feedback().duty);
    }
}
