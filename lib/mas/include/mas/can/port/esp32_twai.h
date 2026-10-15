#pragma once

#if defined(ESP_PLATFORM)

#include <driver/gpio.h>

#include "../port.h"

namespace mas {
namespace can {

class Esp32Twai : public Port {
  public:
    bool begin(gpio_num_t tx, gpio_num_t rx);

    bool send(const Frame& frame) override;
    bool receive(Frame& frame) override;
    uint32_t micros() override;
    uint32_t rxOverflows() override;
};

} // namespace can
} // namespace mas

#endif
