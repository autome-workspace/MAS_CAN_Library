// 汎用ピンの取っ手（API-30、API-31）。can.pin(ch) で受け取る。
// 線の上の形（reserved.md）が未決なので、今は値を覚えるだけで送受信しない。
#pragma once

#include <stdint.h>

#include "../device.h"

namespace mas {
namespace can {

class Pin : public Device {
  public:
    enum Direction : uint8_t { Input, Output };
    enum Type : uint8_t { Digital, Analog };
    enum Pull : uint8_t { NoPull, PullUp, PullDown };

    Pin() = default;

    // ---- 設定（モードに合わない関数は何もしない） ----
    void configMode(Direction dir, Type type); // 2 つの軸を 1 回で設定する
    void configPull(Pull pull);
    void configInvert(bool invert);
    void configDebounce(uint16_t ms);
    void configHoldOnLoss(bool hold);

    // ---- 入力 ----
    bool read() const;  // 生の値 ≥ 128
    float level() const; // 0〜1
    uint8_t raw() const;
    bool rose() const;
    bool fell() const;
    uint32_t changedAt() const; // 主制御の µs
    bool online() const;

    // ---- 出力 ----
    void set(bool on);
    void setLevel(float level); // 0〜1
    void setRaw(uint8_t raw);

  private:
    explicit Pin(detail::DeviceState* s) : Device(s) {}
    friend class Host;
};

} // namespace can
} // namespace mas
