#pragma once

#include <stdint.h>

#include "core/frame.h"

namespace mas {
namespace can {

using Frame = mas_can_frame_t;

class Port {
  public:
    virtual ~Port() = default;
    virtual bool send(const Frame& frame) = 0;
    virtual bool receive(Frame& frame) = 0;
    virtual uint32_t micros() = 0;
    virtual uint32_t rxOverflows() = 0;  // 受信が溢れて失ったフレームの数（数えられなければ 0）
};

} // namespace can
} // namespace mas
