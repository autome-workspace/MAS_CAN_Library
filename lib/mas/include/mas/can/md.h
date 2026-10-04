// MD（モータードライバー）の取っ手（A1）。
//
// can.md(ch) で受け取る。中身（指令の値、FB）はバスが持ち、この取っ手は窓口にすぎない。
// 値はすべて SI（A2）。位置だけは、積み上げたカウント（1 回転 ＝ 65536）。
#pragma once

#include <stdint.h>

namespace mas {
namespace can {

class Bus;

// 位置のカウントと rad の換算（A2）。
constexpr int64_t kCountsPerRev = 65536;
constexpr double countsToRad(int64_t counts) { return counts * (2.0 * 3.14159265358979323846 / kCountsPerRev); }

class Md {
 public:
  Md() = default;
  uint8_t ch() const { return ch_; }

  // ---- 指令（SI。周期送信はバスが行う。最後に呼んだものが続けて送られる） ----
  void setCurrent(float amps);           // A
  void setSpeed(float radps);            // rad/s（出力軸）
  void setDuty(float ratio, bool brake = false);  // -1.0〜+1.0。brake: PWM のオフの間をブレーキにする
  void stop();                           // 止める値を送る（duty 0、空転）

  // 上級者向け: 生の値（LSB は基板の分解能の設定による）
  void setCurrentRaw(int16_t raw);
  void setSpeedRaw(int16_t raw);
  void setDutyRaw(int16_t raw);          // bit 0 が減衰モード、bit 1–15 が duty

  // ---- 保存しない設定（A4。待たずに戻る。ライブラリが覚え、再起動のたびに送り直す） ----
  void setCurrentResolution(float amps_per_lsb);  // 設定 48
  void setSpeedResolution(float radps_per_lsb);   // 設定 49

  // ---- FB（最後に届いた値） ----
  bool online() const;     // FB が途絶えていない
  bool ready() const;      // 保存しない設定が書けたと確かめた（false の間は止める値を送る）
  float current() const;   // A（測れない基板では 0。currentValid() で確かめる）
  float speed() const;     // rad/s
  int64_t position() const;  // 積み上げたカウント（電源投入時が 0）
  float duty() const;      // 実際に出している duty（-1.0〜+1.0）
  bool currentValid() const;   // FB の bit 2 が 0
  bool speedValid() const;     // FB の bit 1 が 0（位置も同じ）
  bool currentLimited() const; // FB の bit 3
  bool stopped() const;        // FB の bit 4
  bool estopLatched() const;   // FB の bit 5

  // ---- 異常（A7） ----
  bool hasFault() const;         // FB の bit 0
  uint32_t faultDetail() const;  // 異常の詳細（ライブラリが読んでおいた値）
  void clearFault();             // 再起動以外の異常の bit を消す

 private:
  friend class Bus;
  Md(Bus *bus, uint8_t ch) : bus_(bus), ch_(ch) {}
  Bus *bus_ = nullptr;
  uint8_t ch_ = 0;
};

}  // namespace can
}  // namespace mas
