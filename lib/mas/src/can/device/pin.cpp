// 汎用ピン、エアー、ソレノイド。線の上の形（reserved.md）が未決なので、今は値を覚えるだけ。
#include "mas/can/device/pin.h"

#include "../state.h"
#include "mas/can/device/air.h"
#include "mas/can/device/solenoid.h"

namespace mas {
namespace can {

using detail::AirState;
using detail::PinState;
using detail::SolenoidState;

namespace {
PinState* pinOf(detail::DeviceState* s) {
    return s && s->kind == Kind::Pin ? static_cast<PinState*>(s) : nullptr;
}
AirState* airOf(detail::DeviceState* s) {
    return s && s->kind == Kind::Air ? static_cast<AirState*>(s) : nullptr;
}
SolenoidState* solenoidOf(detail::DeviceState* s) {
    return s && s->kind == Kind::Solenoid ? static_cast<SolenoidState*>(s) : nullptr;
}
} // namespace

void Pin::configMode(Direction dir, Type type) {
    if (PinState* p = pinOf(s_)) {
        p->dir = dir;
        p->type = type;
    }
}
void Pin::configPull(Pull pull) {
    if (PinState* p = pinOf(s_)) p->pull = pull;
}
void Pin::configInvert(bool invert) {
    if (PinState* p = pinOf(s_)) p->invert = invert;
}
void Pin::configDebounce(uint16_t ms) {
    if (PinState* p = pinOf(s_)) p->debounce_ms = ms;
}
void Pin::configHoldOnLoss(bool hold) {
    if (PinState* p = pinOf(s_)) p->hold = hold;
}

bool Pin::read() const { return raw() >= 128; }
float Pin::level() const { return raw() / 255.0f; }
uint8_t Pin::raw() const { return 0; } // 入力はまだ受けない
bool Pin::rose() const { return false; }
bool Pin::fell() const { return false; }
uint32_t Pin::changedAt() const { return 0; }
bool Pin::online() const { return false; }

void Pin::set(bool on) { setRaw(on ? 255 : 0); }
void Pin::setLevel(float level) {
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    setRaw((uint8_t)(level * 255.0f + 0.5f));
}
void Pin::setRaw(uint8_t raw) {
    PinState* p = pinOf(s_);
    if (p && p->dir == Output) p->out = raw; // モードに合わない呼び出しは何もしない
}

void Air::set(bool on) {
    if (AirState* a = airOf(s_)) a->on = on;
}
bool Air::isOn() const {
    AirState* a = airOf(s_);
    return a && a->on;
}
void Air::configHoldOnLoss(bool hold) {
    if (AirState* a = airOf(s_)) a->hold = hold;
}

void Solenoid::pulse(uint16_t ms) {
    if (SolenoidState* v = solenoidOf(s_)) v->pulse_ms = ms;
}
void Solenoid::on() {
    if (SolenoidState* v = solenoidOf(s_)) v->on = true;
}
void Solenoid::off() {
    if (SolenoidState* v = solenoidOf(s_)) v->on = false;
}

} // namespace can
} // namespace mas
