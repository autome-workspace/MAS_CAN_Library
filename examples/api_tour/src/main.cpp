// ライブラリのAPI例。
// 想定: ESP32-S3 ＋ ATA6561。バスに MD × 6（車輪 4 はエンコーダ付き）、コントローラ、
// サーボ、汎用ピン（リミットスイッチ、ポテンショメータ、ランプ）、エアー、ソレノイド
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;

mcan::Md wheel[4] = {can.md(0), can.md(1), can.md(2), can.md(3)};
mcan::Md lift = can.md(4);
mcan::Md roller = can.md(5);
mcan::Controller pad = can.controller(0);
mcan::Servo arm = can.servo(0);

mcan::Pin limit = can.pin(0);  // lift の上限
mcan::Pin pot = can.pin(1);
mcan::Pin lamp = can.pin(4);
mcan::Air grip = can.air(0);
mcan::Solenoid kicker = can.solenoid(0);

void onFault(mcan::Device &dev, uint32_t new_bits) {
    Serial.printf("ch %d fault 0x%08lx\n", dev.ch(), (unsigned long)new_bits);
    if (dev == lift) lift.setCurrent(0.0f);
}

void setup() {
    Serial.begin(115200);

    if (!can.begin(GPIO_NUM_5, GPIO_NUM_4)) Serial.println(mcan::toString(can.error()));
    can.onFault(onFault);

    for (auto &w : wheel) w.configSpeedResolution(0.01f);  // rad/s per LSB
    lift.configCurrentResolution(0.002f);                  // A per LSB
    pad.configDeadzone(0.05f);
    arm.configCalibration(-1.57f, 1.57f, 600, 2400); // 角度 2 つと、そのときのパルス幅 2 つ
    can.configBusLoadWarning(0.7f);

    limit.configMode(mcan::Pin::Input, mcan::Pin::Digital);
    limit.configPull(mcan::Pin::PullUp);
    limit.configInvert(true); // 押すと 0 のスイッチを、押したら true に
    limit.configDebounce(5);  // ms
    pot.configMode(mcan::Pin::Input, mcan::Pin::Analog);
    lamp.configMode(mcan::Pin::Output, mcan::Pin::Digital);
    grip.configHoldOnLoss(true); // 途絶しても掴んだまま（主制御が死んでもそのまま。C-90）

    Serial.printf("wheel %.1f rad/s, lift %.1f A\n", wheel[0].speedRange(), lift.currentRange());

    if (!can.waitReady(1000)) Serial.println("not ready");
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

    // 電流の指令（A）。上限に当たっていたら上には動かさない
    float up = (pad.r2() - pad.l2()) * 3.0f;
    if (limit.read() && up > 0.0f) up = 0.0f;
    lift.setCurrent(up);
    if (limit.rose()) Serial.printf("limit at %lu us\n", (unsigned long)limit.changedAt());

    // duty の指令（-1〜+1）。離したらブレーキで止める
    if (pad.held(mcan::Button::North)) {
        roller.setDuty(0.6f);
    } else if (pad.pressed(mcan::Button::South)) {
        roller.setDuty(-0.6f);
    } else if (pad.released(mcan::Button::North)) {
        roller.setDuty(0.0f, true);
    }

    // サーボ（rad）
    if (pad.held(mcan::Button::L3)) {
        arm.setAngle(pad.rightY() * 1.2f);
    } else if (pad.pressed(mcan::Button::Select)) {
        arm.release(); // 力を抜く
    }

    // エアーとソレノイド
    if (pad.pressed(mcan::Button::West)) grip.toggle();
    if (pad.pressed(mcan::Button::Right)) kicker.pulse(30);  // ms

    // 汎用ピンの出力
    lamp.set(can.estopped());

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
        Serial.printf("pot %.2f%s, grip %s\n", pot.level(), pot.online() ? "" : " (offline)",
                      grip.isOn() ? "on" : "off");
        Serial.printf("rx overflow %lu, bus %.0f%%\n", (unsigned long)can.rxOverflows(),
                      can.busLoad() * 100.0f);
    }
}
