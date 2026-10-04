// 仮の本体。API の形を確かめるため、何もしない。中身を書いたら、このファイルは消す。
#include "mas/can.h"

namespace mas {
namespace can {

void Bus::begin(Port &port) { port_ = &port; }
void Bus::update() {}
bool Bus::waitOnline(const Md *, size_t, uint32_t) { return false; }
void Bus::estop() {}
void Bus::clearEstop() {}
bool Bus::estopped() const { return false; }
void Bus::onFault(FaultHandler handler) { fault_handler_ = handler; }
void Bus::setSyncSource(bool enable) { sync_source_ = enable; }

void Md::setCurrent(float) {}
void Md::setSpeed(float) {}
void Md::setDuty(float, bool) {}
void Md::stop() {}
void Md::setCurrentRaw(int16_t) {}
void Md::setSpeedRaw(int16_t) {}
void Md::setDutyRaw(int16_t) {}
void Md::setCurrentResolution(float) {}
void Md::setSpeedResolution(float) {}
const Md::Feedback &Md::feedback() const {
  static const Feedback empty{};
  return empty;
}
bool Md::online() const { return false; }
bool Md::ready() const { return false; }
float Md::current() const { return 0; }
float Md::speed() const { return 0; }
int64_t Md::position() const { return 0; }
float Md::duty() const { return 0; }
bool Md::currentValid() const { return false; }
bool Md::speedValid() const { return false; }
bool Md::currentLimited() const { return false; }
bool Md::stopped() const { return true; }
bool Md::estopLatched() const { return false; }
bool Md::hasFault() const { return false; }
uint32_t Md::faultDetail() const { return 0; }
void Md::clearFault() {}

bool Controller::connected() const { return false; }
float Controller::leftX() const { return 0; }
float Controller::leftY() const { return 0; }
float Controller::rightX() const { return 0; }
float Controller::rightY() const { return 0; }
float Controller::l2() const { return 0; }
float Controller::r2() const { return 0; }
int8_t Controller::leftXRaw() const { return 0; }
int8_t Controller::leftYRaw() const { return 0; }
int8_t Controller::rightXRaw() const { return 0; }
int8_t Controller::rightYRaw() const { return 0; }
uint8_t Controller::l2Raw() const { return 0; }
uint8_t Controller::r2Raw() const { return 0; }
bool Controller::held(Button) const { return false; }
bool Controller::pressed(Button) const { return false; }
bool Controller::released(Button) const { return false; }
void Controller::setDeadzone(float) {}

#if defined(ARDUINO_ARCH_ESP32)
bool Esp32Twai::begin(const Pins &) { return false; }
bool Esp32Twai::send(const Frame &) { return false; }
bool Esp32Twai::receive(Frame &) { return false; }
uint32_t Esp32Twai::micros() { return 0; }
#endif

}  // namespace can
}  // namespace mas
