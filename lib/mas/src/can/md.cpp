#include "mas/can/md.h"

#include "mas/can/bus.h"
#include "mas/can/core/md.h"
#include "mas/can/core/setting.h"

namespace mas {
namespace can {

namespace {
constexpr float kTwoPi = 6.28318530718f;
const Md::Feedback kNoFeedback{};
}  // namespace

#define MD_STATE (bus_->md_[ch_])
#define MD_VALID (bus_ && ch_ < Bus::kMdCount)

static void setMode(detail::MdState &m, detail::MdMode mode, float value, bool brake) {
    m.mode = mode;
    m.raw = false;
    m.value = value;
    m.brake = brake;
}

static void setRaw(detail::MdState &m, detail::MdMode mode, int16_t raw) {
    m.mode = mode;
    m.raw = true;
    m.raw_value = raw;
}

void Md::setCurrent(float amps) {
    if (MD_VALID) setMode(MD_STATE, detail::MdMode::Current, amps, false);
}
void Md::setSpeed(float radps) {
    if (MD_VALID) setMode(MD_STATE, detail::MdMode::Speed, radps, false);
}
void Md::setDuty(float ratio, bool brake) {
    if (MD_VALID) setMode(MD_STATE, detail::MdMode::Duty, ratio, brake);
}
void Md::stop() {
    if (!MD_VALID) return;
    detail::MdState &m = MD_STATE;
    m.raw = false;
    m.value = 0;
    m.brake = false;
}
void Md::setCurrentRaw(int16_t raw) {
    if (MD_VALID) setRaw(MD_STATE, detail::MdMode::Current, raw);
}
void Md::setSpeedRaw(int16_t raw) {
    if (MD_VALID) setRaw(MD_STATE, detail::MdMode::Speed, raw);
}
void Md::setDutyRaw(int16_t raw) {
    if (MD_VALID) setRaw(MD_STATE, detail::MdMode::Duty, raw);
}

void Md::setCurrentResolution(float amps_per_lsb) {
    if (!MD_VALID || !(amps_per_lsb > 0)) return;
    detail::MdState &m = MD_STATE;
    m.current_lsb_set = true;
    m.current_lsb_ma = amps_per_lsb * 1000.0f;
    bus_->queueLsbWrites(ch_);
}
void Md::setSpeedResolution(float radps_per_lsb) {
    if (!MD_VALID || !(radps_per_lsb > 0)) return;
    detail::MdState &m = MD_STATE;
    m.speed_lsb_set = true;
    m.speed_lsb_rpm = radps_per_lsb * (60.0f / kTwoPi);
    bus_->queueLsbWrites(ch_);
}

const Md::Feedback &Md::feedback() const { return MD_VALID ? MD_STATE.fb : kNoFeedback; }
bool Md::online() const { return feedback().online; }
bool Md::ready() const { return MD_VALID && bus_->chReady(ch_); }
float Md::current() const { return feedback().current; }
float Md::speed() const { return feedback().speed; }
int64_t Md::position() const { return feedback().position; }
float Md::duty() const { return feedback().duty; }
bool Md::currentValid() const { return feedback().currentValid(); }
bool Md::speedValid() const { return feedback().speedValid(); }
bool Md::currentLimited() const { return feedback().currentLimited(); }
bool Md::stopped() const { return feedback().stopped(); }
bool Md::estopLatched() const { return feedback().estopLatched(); }
bool Md::commandConflict() const { return MD_VALID && MD_STATE.conflict; }

bool Md::hasFault() const { return feedback().hasFault(); }
uint32_t Md::faultDetail() const { return MD_VALID ? MD_STATE.fault_detail : 0; }
void Md::clearFault() {
    if (!MD_VALID || MD_STATE.fault_detail == 0) return;
    detail::SettingReq r{MAS_CAN_KIND_MD,           ch_, MAS_CAN_OP_WRITE,
                         MAS_CAN_ITEM_FAULT_DETAIL, {0}, detail::ReqTag::ClearFault};
    mas_can_value_set_u32(r.value, MD_STATE.fault_detail);
    if (bus_->queueSetting(r)) {
        MD_STATE.fault_detail = 0;
        MD_STATE.fault_known = false;  // まだ続いていれば、次の FB で読み直す
    }
}

}  // namespace can
}  // namespace mas
