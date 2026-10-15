// エアーの取っ手（API-32）。can.air(ch) で受け取る。
// 線の上の形（reserved.md）が未決なので、今は値を覚えるだけで送らない。
#pragma once

#include <stdint.h>

#include "../device.h"

namespace mas {
namespace can {

class Air : public Device {
  public:
    Air() = default;

    void set(bool on);
    void on() { set(true); }
    void off() { set(false); }
    void toggle() { set(!isOn()); }
    bool isOn() const; // 送った値
    void configHoldOnLoss(bool hold);

  private:
    explicit Air(detail::DeviceState* s) : Device(s) {}
    friend class Host;
};

} // namespace can
} // namespace mas
