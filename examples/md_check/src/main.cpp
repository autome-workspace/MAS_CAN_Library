// モーターを 1 つ回しながら、「なぜ回らないか」を表示する例。md_minimal の次に読む。
//
// ライブラリには、様子を確かめる関数がある。下の check() のように上から順に見ると、
// 原因を自分で絞れる。自分のプログラムが動かないときは、check() を写して使う。
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Md motor = can.md(0); // ch 0 の MD

// 上から順に確かめる。最初に当てはまったものが、いちばん疑わしい
const char *check(const mcan::Md &m) {
    // 基板の声が届いていない → 電源、線の H と L、終端抵抗、基板の番号（ch）
    if (!m.online()) return "offline";
    // 緊急停止がかかっている → 理由を確かめてから can.clearEstop()
    if (m.estopLatched()) return "estop";
    // 基板との打ち合わせが済んでいない → 少し待つ。ずっと続くなら基板の側
    if (!m.ready()) return "wait";
    // 同じ 4 個の組（0〜3、4〜7 …）で、指令の種類が混ざっている → 番号を分ける
    if (m.commandConflict()) return "mixed";
    // 基板が異常を知らせている → 中身は faultDetail()。消すのは clearFault()
    if (m.hasFault()) return "fault";
    // 基板が止めている → その基板にできない指令か、update() の間隔が長い
    if (m.stopped()) return "stopped";
    // 指令は届き、基板は従っている → それでも回らないなら、値が小さいか、機構の側
    return "ok";
}

void setup() {
    Serial.begin(115200);
    // RP2040 は 3 つ目の引数で PIO のブロックを選べる（既定 1）: can.begin(CAN_TX, CAN_RX, 0)
    if (!can.begin(CAN_TX, CAN_RX)) Serial.println(mcan::toString(can.error()));
}

void loop() {
    can.update(); // 送受信はこの中。毎回呼ぶ

    motor.setDuty(0.3f);

    static uint32_t last = 0;
    if (millis() - last >= 500) {
        last = millis();
        Serial.printf("%-8s duty %+.2f  fault 0x%08lx  %s\n", check(motor), motor.feedback().duty,
                      (unsigned long)motor.faultDetail(), mcan::toString(can.error()));
    }
}
