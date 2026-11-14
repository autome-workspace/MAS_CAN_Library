// 線につながっている MD を 1 秒ごとに一覧にする（スキャナ）。モーターは動かさない。
//
// 注意:
// - これ自体が主制御として動く。ほかの主制御がつながっている線では使わない
//   （電流と速度の刻みを既定値に書き直すので、相手の指令の大きさが変わる）。
// - サーボの基板は返事をしないので、見つからない。
// - duty をこなせるかは、基板の返事からは分からない（XDrive の中継機はこなせない）。
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

constexpr int kMaxCh = 32; // MD の ch は 0〜31

mcan::Host can;
mcan::Md md[kMaxCh];

void setup() {
    Serial.begin(115200);
    delay(500);
    for (int i = 0; i < kMaxCh; i++) md[i] = can.md((uint8_t)i);
    // RP2040 は 3 つ目の引数で PIO のブロックを選べる（既定 1）: can.begin(CAN_TX, CAN_RX, 0)
    Serial.println(can.begin(CAN_TX, CAN_RX) ? "CAN ok" : mcan::toString(can.error()));
}

void printTable() {
    int found = 0;
    for (int i = 0; i < kMaxCh; i++) {
        if (!md[i].online()) continue;
        found++;
        const mcan::Md::Feedback &fb = md[i].feedback();
        if (!md[i].ready()) { // 刻みを確かめるまでは、こなせる指令を判じられない
            Serial.printf("MD %2d  checking...\n", i);
            continue;
        }
        Serial.printf("MD %2d  current:%-3s speed:%-3s %s%s%s", i, fb.currentValid() ? "yes" : "no",
                      fb.speedValid() ? "yes" : "no", fb.stopped() ? " stopped" : " running",
                      fb.estopLatched() ? " E-STOP" : "", fb.hasFault() ? " FAULT" : "");
        if (fb.hasFault()) Serial.printf(" 0x%08lx", (unsigned long)md[i].faultDetail());
        Serial.println();
    }
    if (found == 0) Serial.println("no MD found (power? CAN H/L? termination? ch?)");
    Serial.printf("-- %d MD, bus %.0f%%, rx overflow %lu\n", found, can.busLoad() * 100.0f,
                  (unsigned long)can.rxOverflows());
}

void loop() {
    can.update();

    static uint32_t last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        printTable();
    }
}
