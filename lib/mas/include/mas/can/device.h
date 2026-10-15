// 取っ手の共通の親（API-20）。中身（指令の値、FB）は Host が確保し、取っ手はそれを指すだけ。
#pragma once

#include <stdint.h>

namespace mas {
namespace can {

namespace detail {
struct DeviceState;
}

enum class Kind : uint8_t { Md, Controller, Servo, Pin, Air, Solenoid };

class Device {
  public:
    // 確保に失敗した取っ手は何もしない（API-6）。そのとき ch() は 255。
    uint8_t ch() const;
    Kind kind() const;

    // 同じ中身を指しているか（API-20）
    bool operator==(const Device& other) const { return s_ == other.s_; }
    bool operator!=(const Device& other) const { return s_ != other.s_; }

  protected:
    explicit Device(detail::DeviceState* s = nullptr) : s_(s) {}
    detail::DeviceState* s_;

    friend class Host;
};

} // namespace can
} // namespace mas
