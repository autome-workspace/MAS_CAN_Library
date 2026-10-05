#include "mas/can/controller.h"

#include "mas/can/bus.h"

namespace mas {
namespace can {

namespace {
const detail::ControllerState kNoPad{};

float scaleAxis(int8_t raw, float deadzone) {
    float v = raw / 127.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (v < deadzone && v > -deadzone) ? 0.0f : v;
}
}  // namespace

#define PAD (bus_ && ch_ < Bus::kControllerCount ? bus_->pad_[ch_] : kNoPad)

bool Controller::connected() const { return PAD.link; }
float Controller::leftX() const { return scaleAxis(PAD.axis[0], PAD.deadzone); }
float Controller::leftY() const { return scaleAxis(PAD.axis[1], PAD.deadzone); }
float Controller::rightX() const { return scaleAxis(PAD.axis[2], PAD.deadzone); }
float Controller::rightY() const { return scaleAxis(PAD.axis[3], PAD.deadzone); }
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

void Controller::setDeadzone(float deadzone) {
    if (bus_ && ch_ < Bus::kControllerCount) bus_->pad_[ch_].deadzone = deadzone;
}

}  // namespace can
}  // namespace mas
