#if defined(ARDUINO_ARCH_RP2040)

#include "mas/can/port/rp2040_can2040.h"

#include <ACAN2040.h>
#include <Arduino.h>

#include <new>

#include "mas/can/host.h"

namespace mas {
namespace can {

namespace {
Rp2040Can2040* g_ports[2] = {nullptr, nullptr}; // PIO の番号ごと（割り込みから引く）
}

bool Rp2040Can2040::begin(uint8_t tx, uint8_t rx, uint8_t pio) {
    if (pio > 1 || can_) return false;
    can_ = new (std::nothrow) ACAN2040(pio, tx, rx, 1000000UL, F_CPU, onCan);
    if (!can_) return false;
    g_ports[pio] = this;
    can_->begin();
    return true;
}

void Rp2040Can2040::onCan(struct can2040* cd, uint32_t notify, struct can2040_msg* msg) {
    if (notify != CAN2040_NOTIFY_RX || !msg || cd->pio_num > 1) return;
    if (Rp2040Can2040* p = g_ports[cd->pio_num]) p->push(msg);
}

void Rp2040Can2040::push(const struct can2040_msg* msg) {
    if (msg->id & CAN2040_ID_EFF) return; // 拡張 ID は使わない
    if ((uint8_t)(head_ - tail_) >= kRxq) {
        overflows_ = overflows_ + 1;
        return;
    }
    Frame& f = rxq_[head_ % kRxq];
    f.id = (uint16_t)(msg->id & 0x7FF);
    f.rtr = (msg->id & CAN2040_ID_RTR) != 0;
    f.dlc = msg->dlc > 8 ? 8 : (uint8_t)msg->dlc;
    for (int i = 0; i < 8; i++) f.data[i] = msg->data[i];
    head_ = head_ + 1;
}

bool Rp2040Can2040::send(const Frame& frame) {
    if (!can_ || !can_->ok_to_send()) return false;
    struct can2040_msg m = {};
    m.id = frame.id;
    if (frame.rtr) m.id |= CAN2040_ID_RTR;
    m.dlc = frame.dlc;
    for (int i = 0; i < 8; i++) m.data[i] = frame.data[i];
    return can_->send_message(&m);
}

bool Rp2040Can2040::receive(Frame& frame) {
    if (tail_ == head_) return false;
    frame = rxq_[tail_ % kRxq];
    tail_ = tail_ + 1;
    return true;
}

uint32_t Rp2040Can2040::micros() { return (uint32_t)::micros(); }

bool Host::begin(int tx, int rx, int pio) {
    static Rp2040Can2040 ports[2];
    if (pio < 0 || pio > 1 || !ports[pio].begin((uint8_t)tx, (uint8_t)rx, (uint8_t)pio)) {
        error_ = Error::PortStartFailed;
        return false;
    }
    return begin(ports[pio]);
}

} // namespace can
} // namespace mas

#endif
