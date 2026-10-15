// ソレノイドの取っ手（API-32）。can.solenoid(ch) で受け取る。
// 線の上の形と、pulse(ms) をどちらが計るか（open.md）が未決なので、今は値を覚えるだけで送らない。
#pragma once

#include <stdint.h>

#include "../device.h"

namespace mas {
namespace can {

class Solenoid : public Device {
  public:
    Solenoid() = default;

    void pulse(uint16_t ms);
    void on();
    void off();

  private:
    explicit Solenoid(detail::DeviceState* s) : Device(s) {}
    friend class Host;
};

} // namespace can
} // namespace mas
