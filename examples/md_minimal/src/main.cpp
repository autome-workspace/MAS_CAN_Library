// MD を 1 つ duty で回すだけの最小の例。
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Md motor = can.md(0); // ch 0 の MD（基板の DIP で決まる）

void setup() {
    Serial.begin(115200);
#if defined(ESP_PLATFORM)
    bool ok = can.begin(GPIO_NUM_5, GPIO_NUM_4); // tx, rx
#else
    bool ok = can.begin(0, 1); // tx, rx（RP2040。PIO は 1）
#endif
    if (!ok) Serial.println(mcan::toString(can.error()));
    if (!can.waitReady(1000)) Serial.println("MD not ready");
}

void loop() {
    can.update(); // 送受信はこの中。毎回呼ぶ

    motor.setDuty(0.3f); // 30%。値を置くだけで、送るのは update()

    static uint32_t last = 0;
    if (millis() - last >= 500) {
        last = millis();
        Serial.printf("%s duty %+.2f\n", motor.online() ? "online" : "offline",
                      motor.feedback().duty);
    }
}
