// ライブラリのAPI例。
// 想定: ESP32-S3 ＋ ATA6561。バスに MD × 4（エンコーダ付き）とコントローラ
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Esp32Twai twai;
mcan::Bus can;

mcan::Md wheel[4] = {can.md(0), can.md(1), can.md(2), can.md(3)};
mcan::Md lift = can.md(4);
mcan::Md roller = can.md(5);
mcan::Controller pad = can.controller(0);

void onFault(mcan::Device &dev, uint32_t new_bits) {
    Serial.printf("ch %d fault 0x%08lx\n", dev.ch(), (unsigned long)new_bits);
    if (dev == lift) lift.setCurrent(0.0f);
}

void setup() {
    Serial.begin(115200);

    twai.begin({GPIO_NUM_5, GPIO_NUM_4});
    if (!can.begin(twai)) Serial.println("out of memory");
    can.onFault(onFault);

    for (auto &w : wheel) w.setSpeedResolution(0.01f);  // rad/s per LSB
    lift.setCurrentResolution(0.002f);                  // A per LSB
    pad.setDeadzone(0.05f);
    Serial.printf("wheel %.1f rad/s, lift %.1f A\n", wheel[0].speedRange(), lift.currentRange());

    if (!can.waitOnline(wheel, 4, 1000)) Serial.println("wheel missing");
}

void loop() {
    can.update();

    // E-stop
    if (pad.held(mcan::Button::L1) && pad.held(mcan::Button::R1)) can.estop();
    if (can.estopped() && pad.pressed(mcan::Button::Up)) can.clearEstop();

    if (!pad.connected()) {
        for (auto &w : wheel) w.stop();
        lift.stop();
        roller.stop();
        return;
    }

    // 速度の指令（rad/s）
    float drive = pad.leftY() * 20.0f;
    float turn = pad.rightX() * 10.0f;
    wheel[0].setSpeed(drive + turn);
    wheel[1].setSpeed(drive - turn);
    wheel[2].setSpeed(drive + turn);
    wheel[3].setSpeed(drive - turn);

    // 電流の指令（A）
    lift.setCurrent((pad.r2() - pad.l2()) * 3.0f);

    // duty の指令（-1〜+1）。離したらブレーキで止める
    if (pad.held(mcan::Button::North)) {
        roller.setDuty(0.6f);
    } else if (pad.pressed(mcan::Button::South)) {
        roller.setDuty(-0.6f);
    } else if (pad.released(mcan::Button::North)) {
        roller.setDuty(0.0f, true);
    }

    // 異常を問い合わせて消す
    if (lift.hasFault() && pad.pressed(mcan::Button::East)) {
        Serial.printf("lift detail 0x%08lx\n", (unsigned long)lift.faultDetail());
        lift.clearFault();
    }

    // FB
    static uint32_t last = 0;
    if (millis() - last >= 100) {
        last = millis();
        for (auto &w : wheel) {
            Serial.printf("[%d] %s%s %6.2f rad/s %5.2f A %8.3f rad%s%s\n", w.ch(),
                          w.online() ? "on" : "--", w.ready() ? "" : "(wait)", w.speed(),
                          w.current(), mcan::countsToRad(w.position()), w.stopped() ? " stop" : "",
                          w.estopLatched() ? " estop" : "");
        }
        const mcan::Md::Feedback &fb = lift.feedback();
        Serial.printf("lift %5.2f A%s duty %+.2f\n", fb.current,
                      fb.currentLimited() ? " (limit)" : "", fb.duty);
        Serial.printf("rx overflow %lu\n", (unsigned long)can.rxOverflows());
    }
}
