// MD をシリアルから試す。
//   ch 3     … 相手の ch を変える（既定 0）
//   d 0.3    … duty（-1〜+1）
//   i 0.5    … 電流 [A]
//   s        … 止める
//   e / c    … E-stop / 解除
//   x        … 異常の bit を消す
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Esp32Twai twai;
mcan::Host can;
mcan::Md md = can.md(0);

void onFault(mcan::Device &dev, uint32_t bits) {
    Serial.printf("! MD %d fault 0x%08lx\n", dev.ch(), (unsigned long)bits);
}

void onSettingError(mcan::Device &dev, uint8_t result) {
    Serial.printf("! MD %d setting failed (%d)\n", dev.ch(), result);
}

void handleLine(String line) {
    line.trim();
    if (line.isEmpty()) return;
    char cmd = line[0];
    float arg = line.substring(1).toFloat();
    switch (cmd) {
        case 'c':
            if (line.startsWith("ch")) {
                md.stop();
                md = can.md((uint8_t)line.substring(2).toInt());
                Serial.printf("ch %d\n", md.ch());
            } else {
                can.clearEstop();
                Serial.println("clear");
            }
            break;
        case 'd':
            md.setDuty(arg);
            Serial.printf("duty %.2f\n", arg);
            break;
        case 'i':
            md.setCurrent(arg);
            Serial.printf("current %.3f A\n", arg);
            break;
        case 's':
            md.stop();
            Serial.println("stop");
            break;
        case 'e':
            can.estop();
            Serial.println("E-stop");
            break;
        case 'x':
            md.clearFault();
            Serial.println("clear fault");
            break;
        default:
            Serial.println("? ch N / d X / i X / s / e / c / x");
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(twai.begin(GPIO_NUM_5, GPIO_NUM_4) ? "TWAI ok" : "TWAI failed");
    can.begin(twai);
    can.onFault(onFault);
    can.onSettingError(onSettingError);
    Serial.println("ch N / d X / i X / s / e / c / x");
}

void loop() {
    can.update();

    static String line;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            handleLine(line);
            line = "";
        } else {
            line += ch;
        }
    }

    static uint32_t last = 0;
    if (millis() - last >= 200) {
        last = millis();
        const auto &fb = md.feedback();
        Serial.printf("[%d] %s%s %6.3f A duty %+.2f%s%s%s%s fault 0x%lx\n", md.ch(),
                      fb.online ? "on" : "--", md.ready() ? "" : "(wait)", fb.current, fb.duty,
                      fb.stopped() ? " stop" : "", fb.estopLatched() ? " estop" : "",
                      fb.hasFault() ? " FAULT" : "", can.estopped() ? " [E]" : "",
                      (unsigned long)md.faultDetail());
    }
}
