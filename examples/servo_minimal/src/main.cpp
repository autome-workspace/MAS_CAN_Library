// サーボを 1 つ角度で動かすだけの最小の例。サーボに FB は無い。
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Servo arm = can.servo(0); // ch 0 のサーボ

void setup() {
    Serial.begin(115200);
    // RP2040 は 3 つ目の引数で PIO のブロックを選べる（既定 1）: can.begin(CAN_TX, CAN_RX, 0)
    if (!can.begin(CAN_TX, CAN_RX)) Serial.println(mcan::toString(can.error()));
    arm.configCalibration(-1.57f, 1.57f, 500, 2500); // -90° で 500 µs、+90° で 2500 µs
}

void loop() {
    can.update();

    float t = millis() * 0.001f;
    arm.setAngle(sinf(t) * 1.0f); // ±1 rad をゆっくり往復
}
