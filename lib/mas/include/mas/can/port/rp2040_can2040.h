// RP2040 の can2040（PIO で作る CAN）のポート。ACAN2040 ライブラリを使う（使う側の
// platformio.ini に入れる。can2040 は GPL-3.0）。PIO 0 と 1 に 1 本ずつ、2 本まで並べられる。
#pragma once

#if defined(ARDUINO_ARCH_RP2040)

#include "../port.h"

class ACAN2040;
struct can2040;
struct can2040_msg;

namespace mas {
namespace can {

class Rp2040Can2040 : public Port {
  public:
    bool begin(uint8_t tx, uint8_t rx, uint8_t pio = 1); // 1 Mbps

    bool send(const Frame& frame) override;
    bool receive(Frame& frame) override;
    uint32_t micros() override;
    uint32_t rxOverflows() override { return overflows_; }

  private:
    static void onCan(struct can2040* cd, uint32_t notify, struct can2040_msg* msg);
    void push(const struct can2040_msg* msg);

    static constexpr uint8_t kRxq = 64; // 受信は割り込みで積み、receive() で取り出す
    ACAN2040* can_ = nullptr;
    Frame rxq_[kRxq] = {};
    volatile uint8_t head_ = 0, tail_ = 0;
    volatile uint32_t overflows_ = 0; // 受信の箱が満杯で捨てた数
};

} // namespace can
} // namespace mas

#endif
