#include "mas/can/device/md.h"

#include "../state.h"
#include "mas/can/core/md_frame.h"
#include "mas/can/core/setting.h"

namespace mas {
namespace can {

using detail::MdMode;
using detail::MdState;

namespace {
constexpr float kTwoPi = 6.28318530718f;
const Md::Feedback kNoFeedback{};

void command(MdState* m, MdMode mode, float value, bool brake) {
    if (!m) return;
    m->mode = mode;
    m->raw = false;
    m->value = value;
    m->brake = brake;
    m->dirty = true;
}

void commandRaw(MdState* m, MdMode mode, int16_t raw) {
    if (!m) return;
    m->mode = mode;
    m->raw = true;
    m->raw_value = raw;
    m->dirty = true;
}
} // namespace

#define MD (detail::asMd(s_))

void Md::setCurrent(float amps) { command(MD, MdMode::Current, amps, true); }
void Md::setSpeed(float radps) { command(MD, MdMode::Speed, radps, true); }
void Md::setDuty(float ratio, bool brake) { command(MD, MdMode::Duty, ratio, brake); }
void Md::setCurrentRaw(int16_t raw) { commandRaw(MD, MdMode::Current, raw); }
void Md::setSpeedRaw(int16_t raw) { commandRaw(MD, MdMode::Speed, raw); }
void Md::setDutyRaw(int16_t raw) { commandRaw(MD, MdMode::Duty, raw); }

void Md::stop() {
    MdState* m = MD;
    if (!m || m->mode == MdMode::None) return;
    m->raw = false;
    m->value = 0;
    m->dirty = true;
}

void Md::configCurrentResolution(float amps_per_lsb) {
    MdState* m = MD;
    if (!m || !(amps_per_lsb > 0)) return;
    m->current.want = amps_per_lsb * 1000.0f; // 基板と違えば Host が空いたときに書きに行く
    m->lsb_hold = false;
}

void Md::configSpeedResolution(float radps_per_lsb) {
    MdState* m = MD;
    if (!m || !(radps_per_lsb > 0)) return;
    m->speed.want = radps_per_lsb * (60.0f / kTwoPi);
    m->lsb_hold = false;
}

float Md::currentRange() const {
    MdState* m = MD;
    return (m ? m->current.want : 1.0f) * 0.001f * 32767.0f;
}

float Md::speedRange() const {
    MdState* m = MD;
    return (m ? m->speed.want : 1.0f) * (kTwoPi / 60.0f) * 32767.0f;
}

const Md::Feedback& Md::feedback() const {
    MdState* m = MD;
    return m ? m->fb : kNoFeedback;
}
bool Md::online() const { return feedback().online; }
bool Md::ready() const {
    MdState* m = MD;
    return m && m->ready();
}
bool Md::stopped() const { return feedback().stopped(); }
bool Md::estopLatched() const { return feedback().estopLatched(); }
bool Md::commandConflict() const {
    MdState* m = MD;
    return m && m->conflict;
}
float Md::current() const { return feedback().current; }
float Md::speed() const { return feedback().speed; }
int64_t Md::position() const { return feedback().position; }

bool Md::hasFault() const { return feedback().hasFault(); }
uint32_t Md::faultDetail() const {
    MdState* m = MD;
    return m ? m->fault_detail : 0;
}

void Md::clearFault() {
    MdState* m = MD;
    if (!m || m->fault_detail == 0) return;
    detail::SettingReq r;
    r.dev = m;
    r.op = MAS_CAN_OP_WRITE;
    r.item = MAS_CAN_ITEM_FAULT_DETAIL;
    r.tag = detail::ReqTag::ClearFault;
    mas_can_value_set_u32(r.value, m->fault_detail);
    if (m->host->queueSetting(r)) {
        m->fault_detail = 0;
        m->fault_known = false; // まだ続いていれば、次の FB で読み直す
    }
}

} // namespace can
} // namespace mas
