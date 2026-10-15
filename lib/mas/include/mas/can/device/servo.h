// サーボの取っ手（API-55〜57）。can.servo(ch) で受け取る。FB は無い。
#pragma once

#include <stdint.h>

#include "../device.h"

namespace mas {
namespace can {

class Servo : public Device {
  public:
    Servo() = default;

    void setPulseUs(uint16_t us); // 10 µs 刻みに丸め、500〜2500 µs に抑える
    void setAngle(float rad);     // configCalibration の 2 点を結ぶ直線でパルス幅に直す
    void release();               // パルスを止めて力を抜く

    // 角度 rad0 で us0、rad1 で us1（反転は 2 点の向きで表す）
    void configCalibration(float rad0, float rad1, uint16_t us0, uint16_t us1);

  private:
    explicit Servo(detail::DeviceState* s) : Device(s) {}
    friend class Host;
};

} // namespace can
} // namespace mas
