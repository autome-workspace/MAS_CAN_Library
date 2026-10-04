// ESP32 の TWAI のポート。ESP32 で使うときだけ include する。
#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include <driver/gpio.h>

#include "port.h"

namespace mas {
namespace can {

class Esp32Twai : public Port {
 public:
  struct Pins {
    gpio_num_t tx;
    gpio_num_t rx;
  };

  // TWAI を 1 Mbps（サンプル点 80%、SJW は取れる最大。D7）で始める。失敗したら false。
  bool begin(const Pins &pins);

  bool send(const Frame &frame) override;
  bool receive(Frame &frame) override;
  uint32_t micros() override;
};

}  // namespace can
}  // namespace mas

#endif
