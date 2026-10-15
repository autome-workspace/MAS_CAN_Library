// MAS CAN（プロトコル v1）の主制御のライブラリの入口。
//
//   #include <mas/can.h>
//   mas::can::Host can;
//   mas::can::Md wheel = can.md(0);
//
// API の正典は docs/decisions/api.md。
#pragma once

#include "can/host.h"

#if defined(ESP_PLATFORM)
#include "can/port/esp32_twai.h"
#elif defined(ARDUINO_ARCH_RP2040)
#include "can/port/rp2040_can2040.h"
#endif
