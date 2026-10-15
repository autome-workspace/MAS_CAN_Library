// 取っ手の中身（ライブラリの中だけで使う）。Host が確保し、連結リストでたどる。
#pragma once

#include <stdint.h>

#include "mas/can/host.h"

namespace mas {
namespace can {
namespace detail {

struct DeviceState {
    Host* host = nullptr;
    DeviceState* next = nullptr;
    Kind kind = Kind::Md;
    uint8_t ch = 0;
};

enum class MdMode : uint8_t { None, Current, Speed, Duty };

struct MdState : DeviceState {
    // 指令
    MdMode mode = MdMode::None;
    bool raw = false;
    float value = 0; // SI（A、rad/s、duty の割合）
    bool brake = true;
    int16_t raw_value = 0;
    bool dirty = false;    // flush() で送る
    bool conflict = false; // API-52

    // プログラムの値（API-14、API-43、API-44）。基板に書けたと確かめるまで pending
    float current_lsb_ma = 1.0f, speed_lsb_rpm = 1.0f;
    bool current_pending = true, speed_pending = true;
    bool lsb_hold = false; // 失敗したので lsb_retry_us から少し待つ
    uint32_t lsb_retry_us = 0;

    // FB
    Md::Feedback fb;
    bool fb_seen = false;
    uint32_t last_raw_pos = 0;
    bool relink = false; // 基板の再起動の後、次の FB で位置を結び直す
    bool fault_known = false, fault_reading = false;
    uint32_t fault_detail = 0;
};

struct ControllerState : DeviceState {
    int8_t axis[4] = {0, 0, 0, 0};
    uint8_t trigger[2] = {0, 0};
    uint16_t buttons = 0, pressed = 0, released = 0;
    bool link = false, seen = false, online = false;
    uint32_t last_us = 0;
    float deadzone = 0;
};

struct ServoState : DeviceState {
    uint8_t value = MAS_CAN_SERVO_STOP;
    bool dirty = false;
    // 2 点の較正（API-56）。既定は ±90° で 500〜2500 µs
    float rad0 = -1.5707963f, rad1 = 1.5707963f;
    uint16_t us0 = 500, us1 = 2500;
};

// 汎用ピン、エアー、ソレノイドは線の上の形が未決なので、値を覚えるだけ
struct PinState : DeviceState {
    uint8_t dir = Pin::Input, type = Pin::Digital, pull = Pin::NoPull;
    bool invert = false, hold = false;
    uint16_t debounce_ms = 0;
    uint8_t out = 0;
};

struct AirState : DeviceState {
    bool on = false, hold = false;
};

struct SolenoidState : DeviceState {
    bool on = false;
    uint16_t pulse_ms = 0;
};

inline MdState* asMd(DeviceState* s) {
    return s && s->kind == Kind::Md ? static_cast<MdState*>(s) : nullptr;
}

} // namespace detail
} // namespace can
} // namespace mas
