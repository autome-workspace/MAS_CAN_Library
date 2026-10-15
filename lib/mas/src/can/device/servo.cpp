#include "mas/can/device/servo.h"

#include "../state.h"

namespace mas {
namespace can {

using detail::ServoState;

namespace {
ServoState* servo(detail::DeviceState* s) {
    return s && s->kind == Kind::Servo ? static_cast<ServoState*>(s) : nullptr;
}
} // namespace

// パルス幅 ＝ 500 ＋ 値 × 10 µs（値 0〜200。S-2）
void Servo::setPulseUs(uint16_t us) {
    ServoState* v = servo(s_);
    if (!v) return;
    if (us < 500) us = 500;
    if (us > 2500) us = 2500;
    v->value = (uint8_t)((us - 500 + 5) / 10);
    v->dirty = true;
}

void Servo::setAngle(float rad) {
    ServoState* v = servo(s_);
    if (!v || v->rad1 == v->rad0) return;
    float us = v->us0 + (rad - v->rad0) * ((float)v->us1 - v->us0) / (v->rad1 - v->rad0);
    if (us < 0) us = 0;
    if (us > 65535.0f) us = 65535.0f;
    setPulseUs((uint16_t)(us + 0.5f));
}

void Servo::release() {
    ServoState* v = servo(s_);
    if (!v) return;
    v->value = MAS_CAN_SERVO_STOP;
    v->dirty = true;
}

void Servo::configCalibration(float rad0, float rad1, uint16_t us0, uint16_t us1) {
    ServoState* v = servo(s_);
    if (!v || rad0 == rad1) return;
    v->rad0 = rad0;
    v->rad1 = rad1;
    v->us0 = us0;
    v->us1 = us1;
}

} // namespace can
} // namespace mas
