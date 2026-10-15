// MD（モータードライバー）の取っ手。can.md(ch) で受け取る。
// 値は SI（API-12）。位置だけは積み上げたカウント（1 回転 ＝ 65536。API-13）。
#pragma once

#include <stdint.h>

#include "../device.h"

namespace mas {
namespace can {

constexpr int64_t kCountsPerRev = 65536;
constexpr double countsToRad(int64_t counts) {
    return counts * (2.0 * 3.14159265358979323846 / kCountsPerRev);
}

class Md : public Device {
  public:
    // FB のまとめ（API-25）。中身が変わるのは update() の中だけ。
    struct Feedback {
        float current = 0;       // A（currentValid() が false なら 0）
        float speed = 0;         // rad/s
        int64_t position = 0;    // 積み上げたカウント（電源投入時が 0）
        float duty = 0;          // 実際に出している duty（-1〜+1。測れる量で埋まっていれば 0）
        uint8_t status = 0;      // 状態の byte（M-18）
        uint32_t receivedUs = 0; // 最後に受けた時刻
        bool online = false;     // FB が 200 ms 以内に来ている

        bool hasFault() const { return status & 0x01; }
        bool speedValid() const { return !(status & 0x02); }
        bool currentValid() const { return !(status & 0x04); }
        bool currentLimited() const { return status & 0x08; }
        bool stopped() const { return status & 0x10; }
        bool estopLatched() const { return status & 0x20; }
    };

    Md() = default;

    // ---- 指令（API-50。値を置くだけで、送るのは update() か flush()） ----
    void setCurrent(float amps);
    void setSpeed(float radps);
    void setDuty(float ratio, bool brake = true); // API-40
    void stop();                                  // 今の指令の種類のまま止める値を送る
    void setCurrentRaw(int16_t raw);
    void setSpeedRaw(int16_t raw);
    void setDutyRaw(int16_t raw); // bit 0 がブレーキ、bit 1–15 が duty

    // ---- プログラムの値（API-14、API-58。待たずに戻る） ----
    void configCurrentResolution(float amps_per_lsb);
    void configSpeedResolution(float radps_per_lsb);
    float currentRange() const; // その刻みで送れる最大（A）
    float speedRange() const;   // その刻みで送れる最大（rad/s）

    // ---- 状態と FB（API-51） ----
    const Feedback& feedback() const;
    bool online() const;
    bool ready() const; // プログラムの値を書けたと確かめた（false の間は止める値を送る。API-43）
    bool stopped() const;
    bool estopLatched() const;
    bool commandConflict() const; // API-52
    float current() const;
    float speed() const;
    int64_t position() const;

    // ---- 異常（API-18、API-19） ----
    bool hasFault() const;
    uint32_t faultDetail() const;
    void clearFault();

  private:
    explicit Md(detail::DeviceState* s) : Device(s) {}
    friend class Host;
};

} // namespace can
} // namespace mas
