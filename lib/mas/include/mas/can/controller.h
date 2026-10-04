// コントローラの取っ手（A1、A6）。can.controller(ch) で受け取る。
#pragma once

#include <stdint.h>

namespace mas {
namespace can {

class Bus;

// ボタンは位置で名前を付ける（controller-draft.md）。値は状態のフレームの bit の番号。
enum class Button : uint8_t {
  Up = 0, Down = 1, Left = 2, Right = 3,        // 十字キー
  North = 4, South = 5, West = 6, East = 7,     // 右側のボタン（△/Y、×/A、□/X、○/B）
  L1 = 8, R1 = 9, L3 = 10, R3 = 11,
};

class Controller {
 public:
  Controller() = default;
  uint8_t ch() const { return ch_; }

  // 無線が切れたか、200 ms フレームが来なければ false。そのときスティックは 0、ボタンは離した状態。
  bool connected() const;

  // スティックは -1.0〜+1.0（右と上が正）、トリガーは 0.0〜1.0。
  float leftX() const;
  float leftY() const;
  float rightX() const;
  float rightY() const;
  float l2() const;
  float r2() const;

  // 生の値
  int8_t leftXRaw() const;
  int8_t leftYRaw() const;
  int8_t rightXRaw() const;
  int8_t rightYRaw() const;
  uint8_t l2Raw() const;
  uint8_t r2Raw() const;

  bool held(Button b) const;      // 押されている
  bool pressed(Button b) const;   // 前回の update() から今回までに、押された瞬間があった
  bool released(Button b) const;  // 同じく、離された瞬間があった

  // スティックの遊び。この大きさ未満を 0 にする（既定は 0 ＝ 何もしない）。
  void setDeadzone(float deadzone);

 private:
  friend class Bus;
  Controller(Bus *bus, uint8_t ch) : bus_(bus), ch_(ch) {}
  Bus *bus_ = nullptr;
  uint8_t ch_ = 0;
};

}  // namespace can
}  // namespace mas
