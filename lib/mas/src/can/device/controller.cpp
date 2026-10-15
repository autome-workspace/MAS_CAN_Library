#include "mas/can/device/controller.h"

#include <math.h>

#include "../state.h"

namespace mas {
namespace can {

using detail::ControllerState;

namespace {
const ControllerState kNoPad{};

const ControllerState& pad(detail::DeviceState* s) {
    return s && s->kind == Kind::Controller ? *static_cast<ControllerState*>(s) : kNoPad;
}

// スティック 1 本（x, y）を円の形で切り、境目から 0 で始めて 1 まで伸ばす（API-29）
float stick(const ControllerState& p, int x_axis, bool want_y) {
    float x = p.axis[x_axis] / 127.0f, y = p.axis[x_axis + 1] / 127.0f;
    float r = sqrtf(x * x + y * y);
    float dz = p.deadzone;
    if (dz > 0) {
        if (r <= dz) return 0.0f;
        float k = ((r > 1.0f ? 1.0f : r) - dz) / (1.0f - dz) / r;
        x *= k;
        y *= k;
    }
    float v = want_y ? y : x;
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}
} // namespace

#define PAD (pad(s_))

bool Controller::online() const { return PAD.online; }
bool Controller::connected() const { return PAD.online && PAD.link; }
float Controller::leftX() const { return stick(PAD, 0, false); }
float Controller::leftY() const { return stick(PAD, 0, true); }
float Controller::rightX() const { return stick(PAD, 2, false); }
float Controller::rightY() const { return stick(PAD, 2, true); }
float Controller::l2() const { return PAD.trigger[0] / 255.0f; }
float Controller::r2() const { return PAD.trigger[1] / 255.0f; }
int8_t Controller::leftXRaw() const { return PAD.axis[0]; }
int8_t Controller::leftYRaw() const { return PAD.axis[1]; }
int8_t Controller::rightXRaw() const { return PAD.axis[2]; }
int8_t Controller::rightYRaw() const { return PAD.axis[3]; }
uint8_t Controller::l2Raw() const { return PAD.trigger[0]; }
uint8_t Controller::r2Raw() const { return PAD.trigger[1]; }
bool Controller::held(Button b) const { return PAD.buttons & (1u << (uint8_t)b); }
bool Controller::pressed(Button b) const { return PAD.pressed & (1u << (uint8_t)b); }
bool Controller::released(Button b) const { return PAD.released & (1u << (uint8_t)b); }

void Controller::configDeadzone(float radius) {
    if (s_ && s_->kind == Kind::Controller && radius >= 0 && radius < 1)
        static_cast<ControllerState*>(s_)->deadzone = radius;
}

} // namespace can
} // namespace mas
