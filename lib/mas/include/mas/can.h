// MAS CAN（プロトコル v1）の入口。
//
//   #include <mas/can.h>
//   mas::can::Bus can;
//   mas::can::Md wheel = can.md(0);
//
// API の決定は docs/redesign/library-api-draft.md。
#pragma once

#include "can/bus.h"
#include "can/controller.h"
#include "can/md.h"
#include "can/port.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "can/esp32_twai.h"
#endif
