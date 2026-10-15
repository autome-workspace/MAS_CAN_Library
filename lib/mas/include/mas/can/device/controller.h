// コントローラの取っ手（API-26〜29、API-53、API-54）。can.controller(ch) で受け取る。
#pragma once

#include <stdint.h>

#include "../device.h"

namespace mas {
namespace can {

// 位置で名前を付ける。値は状態のフレームの bit の番号。
enum class Button : uint8_t {
    Up = 0,
    Down = 1,
    Left = 2,
    Right = 3,
    North = 4, // △/Y
    South = 5, // ×/A
    West = 6,  // □/X
    East = 7,  // ○/B
    L1 = 8,
    R1 = 9,
    L3 = 10,
    R3 = 11,
    Select = 14, // 中央の左（Share、View、−）
    Start = 15,  // 中央の右（Options、Menu、＋）
};

class Controller : public Device {
  public:
    Controller() = default;

    bool online() const;    // 受信機の基板から 200 ms 以内にフレームが来ている
    bool connected() const; // パッドとの無線がつながっている（切れたらスティック 0、ボタンは離す）

    // スティックは -1〜+1（右と上が正）、トリガーは 0〜1
    float leftX() const;
    float leftY() const;
    float rightX() const;
    float rightY() const;
    float l2() const;
    float r2() const;
    int8_t leftXRaw() const;
    int8_t leftYRaw() const;
    int8_t rightXRaw() const;
    int8_t rightYRaw() const;
    uint8_t l2Raw() const;
    uint8_t r2Raw() const;

    bool held(Button b) const;
    bool pressed(Button b) const;  // 前回の update() から押された瞬間があった
    bool released(Button b) const; // 離された瞬間があった

    // スティックごとに円の形で切り、境目から 0 で始めて 1 まで伸ばし直す（既定は 0 ＝ 無し）
    void configDeadzone(float radius);

  private:
    explicit Controller(detail::DeviceState* s) : Device(s) {}
    friend class Host;
};

} // namespace can
} // namespace mas
