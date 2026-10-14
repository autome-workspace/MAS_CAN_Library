#include <Arduino.h>
#include <Servo.h>
#include <ACAN2040.h>

#include <hardware/gpio.h>
#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>
#include <hardware/pwm.h>

namespace {

// CAN_Servo.net: ATA6561 TXD=GP0, RXD=GP1.
constexpr uint8_t kCanTxPin = 0;
constexpr uint8_t kCanRxPin = 1;
constexpr uint8_t kCanPio = 1;  // Leave PIO0 available for the Servo library.
constexpr uint32_t kCanBitrate = 1000000UL;
constexpr uint8_t kStatusLedPin = 16;  // On-board WS2812B on RP2040-Zero.

// CAN_Servo.net: J2..J9 signal pins (GN1..GN8).
constexpr uint8_t kServoPins[] = {29, 28, 27, 26, 15, 14, 13, 12};
constexpr size_t kServoCount = sizeof(kServoPins) / sizeof(kServoPins[0]);

// CAN_Servo.net: U4..U7 input pairs, corresponding to J1/J10/J11/J12.
constexpr uint8_t kMotorPins[][2] = {{10, 11}, {8, 9}, {6, 7}, {4, 5}};
constexpr size_t kMotorCount = sizeof(kMotorPins) / sizeof(kMotorPins[0]);
constexpr uint16_t kMotorPwmTop =
    static_cast<uint16_t>((F_CPU / 20000UL) - 1UL);  // Approximately 20 kHz.

#ifndef SERVO_CAN_GROUP
#define SERVO_CAN_GROUP 20
#endif

#ifndef MOTOR_CAN_GROUP
#define MOTOR_CAN_GROUP 12
#endif

static_assert(SERVO_CAN_GROUP >= 20 && SERVO_CAN_GROUP <= 23,
              "SERVO_CAN_GROUP must be in the protocol range 20..23");
static_assert(MOTOR_CAN_GROUP >= 12 && MOTOR_CAN_GROUP <= 15,
              "MOTOR_CAN_GROUP must be in the protocol range 12..15");
static_assert(kServoCount == 8, "The servo command frame must have eight slots");
static_assert(kMotorCount == 4, "The duty command frame must have four slots");

constexpr uint16_t kEstopId = 0x000;
constexpr uint16_t kEstopClearId = 0x001;
constexpr uint16_t kServoCommandId =
    (2U << 8U) | (static_cast<uint16_t>(SERVO_CAN_GROUP) << 3U);
constexpr uint16_t kMotorCommandId =
    (2U << 8U) | (static_cast<uint16_t>(MOTOR_CAN_GROUP) << 3U);
constexpr uint32_t kCommandTimeoutMs = 100;
constexpr uint16_t kMinPulseUs = 500;
constexpr uint16_t kMaxPulseUs = 2500;
constexpr uint8_t kPulseDisable = 255;

Servo servos[kServoCount];
bool servoAttached[kServoCount] = {};
bool motorPwmConfigured = false;

enum class StatusLed : uint8_t {
  Startup,
  CommandReceived,
  Estop,
};

// Written by the CAN IRQ callback and read by loop(). Access is protected by
// noInterrupts()/interrupts() whenever multiple fields must be consistent.
volatile bool estopLatched = false;
volatile bool outputResetPending = false;
volatile bool servoCommandPending = false;
volatile bool validServoCommandSeen = false;
volatile uint32_t lastServoCommandMs = 0;
volatile uint8_t pendingServoCommand[kServoCount] = {};
volatile bool motorCommandPending = false;
volatile bool validMotorCommandSeen = false;
volatile uint32_t lastMotorCommandMs = 0;
volatile int16_t pendingMotorCommand[kMotorCount] = {};
StatusLed displayedStatusLed = StatusLed::Startup;

void canCallback(struct can2040 *controller, uint32_t notify,
                 struct can2040_msg *message);

ACAN2040 canBus(kCanPio, kCanTxPin, kCanRxPin, kCanBitrate, F_CPU,
                canCallback);

void reserveCanPioStateMachines() {
  // can2040 drives all four state machines directly and therefore does not
  // register them with the Pico SDK's claim API.  The Arduino Servo library
  // uses that API to select a PIO; without these claims it sees PIO1 as free,
  // takes SM0, and stops CAN as soon as the first servo is attached.
  for (uint sm = 0; sm < 4; ++sm) {
    if (!pio_sm_is_claimed(pio1, sm)) {
      pio_sm_claim(pio1, sm);
    }
  }
}

void setStartupLed(uint8_t red, uint8_t green, uint8_t blue) {
  // Standard WS2812 PIO waveform: 10 PIO clocks per 800 kHz data bit.
  const uint16_t instructions[] = {
      static_cast<uint16_t>(pio_encode_out(pio_x, 1) |
                            pio_encode_sideset(1, 0) | pio_encode_delay(2)),
      static_cast<uint16_t>(pio_encode_jmp_not_x(3) |
                            pio_encode_sideset(1, 1) | pio_encode_delay(1)),
      static_cast<uint16_t>(pio_encode_jmp(0) | pio_encode_sideset(1, 1) |
                            pio_encode_delay(4)),
      static_cast<uint16_t>(pio_encode_nop() | pio_encode_sideset(1, 0) |
                            pio_encode_delay(4)),
  };
  const pio_program_t program = {instructions, 4, -1, 0};
  PIO pio = pio0;
  const uint sm = pio_claim_unused_sm(pio, true);
  const uint offset = pio_add_program(pio, &program);

  pio_sm_config config = pio_get_default_sm_config();
  sm_config_set_wrap(&config, offset, offset + 3);
  sm_config_set_sideset(&config, 1, false, false);
  sm_config_set_sideset_pins(&config, kStatusLedPin);
  sm_config_set_out_shift(&config, false, true, 24);
  sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(
      &config, static_cast<float>(clock_get_hz(clk_sys)) / 8000000.0f);

  pio_gpio_init(pio, kStatusLedPin);
  pio_sm_set_consecutive_pindirs(pio, sm, kStatusLedPin, 1, true);
  pio_sm_init(pio, sm, offset, &config);
  pio_sm_set_enabled(pio, sm, true);

  // WS2812 uses GRB byte order. A single transmission remains latched, so the
  // PIO state machine can be released before CAN and servo setup.
  const uint32_t grb = (static_cast<uint32_t>(green) << 16U) |
                       (static_cast<uint32_t>(red) << 8U) | blue;
  pio_sm_put_blocking(pio, sm, grb << 8U);
  while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
    tight_loop_contents();
  }
  delayMicroseconds(100);

  pio_sm_set_enabled(pio, sm, false);
  pio_sm_unclaim(pio, sm);
  pio_remove_program(pio, &program, offset);
}

// This function is IRQ-safe. Taking the pins away from the Servo PIO ends the
// drive pulse immediately; loop() subsequently detaches the Servo objects.
void forceDrivePinsLowFromIrq() {
  for (const uint8_t pin : kServoPins) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, false);
  }
  for (const auto &pair : kMotorPins) {
    for (const uint8_t pin : pair) {
      gpio_set_function(pin, GPIO_FUNC_SIO);
      gpio_set_dir(pin, GPIO_OUT);
      gpio_put(pin, false);
    }
  }
}

void stopAllServos() {
  for (size_t i = 0; i < kServoCount; ++i) {
    if (servoAttached[i]) {
      servos[i].detach();
      servoAttached[i] = false;
    }
    pinMode(kServoPins[i], OUTPUT);
    digitalWrite(kServoPins[i], LOW);
  }
}

void stopAllMotors() {
  for (const auto &pair : kMotorPins) {
    for (const uint8_t pin : pair) {
      gpio_set_function(pin, GPIO_FUNC_SIO);
      gpio_set_dir(pin, GPIO_OUT);
      gpio_put(pin, false);
    }
  }
  motorPwmConfigured = false;
}

void configureMotorPwm() {
  if (motorPwmConfigured) {
    return;
  }

  for (const auto &pair : kMotorPins) {
    const uint slice = pwm_gpio_to_slice_num(pair[0]);
    pwm_set_enabled(slice, false);
    pwm_set_clkdiv(slice, 1.0f);
    pwm_set_wrap(slice, kMotorPwmTop);
    pwm_set_gpio_level(pair[0], 0);
    pwm_set_gpio_level(pair[1], 0);
    gpio_set_function(pair[0], GPIO_FUNC_PWM);
    gpio_set_function(pair[1], GPIO_FUNC_PWM);
    pwm_set_enabled(slice, true);
  }
  motorPwmConfigured = true;
}

uint16_t motorPwmLevel(int16_t duty) {
  // The protocol specifies a signed int16 ratio without a separate scale.
  // Therefore the complete magnitude range maps linearly to 0..100% PWM.
  const uint32_t magnitude =
      duty < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(duty))
               : static_cast<uint32_t>(duty);
  return static_cast<uint16_t>((magnitude * kMotorPwmTop) / 32768UL);
}

void applyMotorCommand(const int16_t *command) {
  configureMotorPwm();
  for (size_t i = 0; i < kMotorCount; ++i) {
    const uint8_t pinA = kMotorPins[i][0];
    const uint8_t pinB = kMotorPins[i][1];
    const uint16_t level = motorPwmLevel(command[i]);

    // Clear both sides before changing direction to avoid shoot-through.
    pwm_set_gpio_level(pinA, 0);
    pwm_set_gpio_level(pinB, 0);
    if (command[i] > 0) {
      pwm_set_gpio_level(pinA, level);
    } else if (command[i] < 0) {
      pwm_set_gpio_level(pinB, level);
    }
  }
}

int16_t readBigEndianInt16(const uint8_t *data) {
  const uint16_t bits =
      (static_cast<uint16_t>(data[0]) << 8U) | data[1];
  return static_cast<int16_t>(bits);
}

bool isValidServoFrame(const can2040_msg &message) {
  if (message.dlc != kServoCount) {
    return false;
  }

  for (size_t i = 0; i < kServoCount; ++i) {
    const uint8_t value = message.data[i];
    if (value > 200 && value != kPulseDisable) {
      return false;  // 201..254 are reserved.
    }
  }
  return true;
}

void applyServoCommand(const uint8_t *command) {
  for (size_t i = 0; i < kServoCount; ++i) {
    const uint8_t value = command[i];
    if (value == kPulseDisable) {
      if (servoAttached[i]) {
        servos[i].detach();
        servoAttached[i] = false;
      }
      pinMode(kServoPins[i], OUTPUT);
      digitalWrite(kServoPins[i], LOW);
      continue;
    }

    if (!servoAttached[i]) {
      const int attachedPin =
          servos[i].attach(kServoPins[i], kMinPulseUs, kMaxPulseUs);
      servoAttached[i] = attachedPin >= 0;
      if (!servoAttached[i]) {
        pinMode(kServoPins[i], OUTPUT);
        digitalWrite(kServoPins[i], LOW);
        continue;
      }
    }
    servos[i].writeMicroseconds(kMinPulseUs +
                                static_cast<uint16_t>(value) * 10U);
  }
}

void canCallback(struct can2040 *controller, uint32_t notify,
                 struct can2040_msg *message) {
  (void)controller;
  if (notify != CAN2040_NOTIFY_RX || message == nullptr) {
    return;
  }

  // Only standard 11-bit data frames are part of this protocol.
  if ((message->id & (CAN2040_ID_EFF | CAN2040_ID_RTR)) != 0) {
    return;
  }
  const uint16_t id = static_cast<uint16_t>(message->id & 0x7ffU);

  // The receiver deliberately does not inspect DLC for E-stop ID 0x000.
  if (id == kEstopId) {
    estopLatched = true;
    outputResetPending = true;
    servoCommandPending = false;
    motorCommandPending = false;
    validServoCommandSeen = false;
    validMotorCommandSeen = false;
    forceDrivePinsLowFromIrq();
    return;
  }

  if (id == kEstopClearId) {
    if (message->dlc == 3 && message->data[0] == 'c' &&
        message->data[1] == 'l' && message->data[2] == 'r') {
      estopLatched = false;
      servoCommandPending = false;
      motorCommandPending = false;
      validServoCommandSeen = false;
      validMotorCommandSeen = false;
    }
    return;
  }

  if (estopLatched) {
    return;
  }

  if (id == kServoCommandId && isValidServoFrame(*message)) {
    for (size_t i = 0; i < kServoCount; ++i) {
      pendingServoCommand[i] = message->data[i];
    }
    lastServoCommandMs = millis();
    validServoCommandSeen = true;
    servoCommandPending = true;
    return;
  }

  if (id == kMotorCommandId && message->dlc == 8) {
    for (size_t i = 0; i < kMotorCount; ++i) {
      pendingMotorCommand[i] = readBigEndianInt16(&message->data[i * 2]);
    }
    lastMotorCommandMs = millis();
    validMotorCommandSeen = true;
    motorCommandPending = true;
  }
}

}  // namespace

void setup() {
  stopAllServos();
  stopAllMotors();
  canBus.begin();
  reserveCanPioStateMachines();
  setStartupLed(0, 12, 0);
}

void loop() {
  uint8_t servoCommand[kServoCount];
  int16_t motorCommand[kMotorCount];
  bool haveServoCommand = false;
  bool haveMotorCommand = false;
  bool stoppedByEstop;
  bool resetOutputs;
  bool seenServoCommand;
  bool seenMotorCommand;
  uint32_t servoReceivedAt;
  uint32_t motorReceivedAt;

  noInterrupts();
  stoppedByEstop = estopLatched;
  resetOutputs = outputResetPending;
  outputResetPending = false;
  seenServoCommand = validServoCommandSeen;
  seenMotorCommand = validMotorCommandSeen;
  servoReceivedAt = lastServoCommandMs;
  motorReceivedAt = lastMotorCommandMs;
  if (!stoppedByEstop && servoCommandPending) {
    for (size_t i = 0; i < kServoCount; ++i) {
      servoCommand[i] = pendingServoCommand[i];
    }
    servoCommandPending = false;
    haveServoCommand = true;
  }
  if (!stoppedByEstop && motorCommandPending) {
    for (size_t i = 0; i < kMotorCount; ++i) {
      motorCommand[i] = pendingMotorCommand[i];
    }
    motorCommandPending = false;
    haveMotorCommand = true;
  }
  interrupts();

  const uint32_t now = millis();
  const bool servoTimedOut =
      !seenServoCommand ||
      static_cast<uint32_t>(now - servoReceivedAt) >= kCommandTimeoutMs;
  const bool motorTimedOut =
      !seenMotorCommand ||
      static_cast<uint32_t>(now - motorReceivedAt) >= kCommandTimeoutMs;

  // Blue only while valid commands are arriving. When the tester's deadman
  // button is released, the protocol timeout returns the LED to green.
  const StatusLed nextStatusLed =
      stoppedByEstop
          ? StatusLed::Estop
          : ((!servoTimedOut || !motorTimedOut) ? StatusLed::CommandReceived
                                                : StatusLed::Startup);
  if (nextStatusLed != displayedStatusLed) {
    displayedStatusLed = nextStatusLed;
    switch (displayedStatusLed) {
      case StatusLed::Startup:
        setStartupLed(0, 12, 0);  // Green: idle / E-stop cleared.
        break;
      case StatusLed::CommandReceived:
        setStartupLed(0, 0, 16);  // Blue: valid commands are being received.
        break;
      case StatusLed::Estop:
        setStartupLed(16, 0, 0);  // Red: E-stop latched.
        break;
    }
  }

  // An E-stop steals the pins from the Servo PIO in IRQ context. Always
  // detach once before a post-clear command is allowed to reattach them.
  if (resetOutputs) {
    stopAllServos();
    stopAllMotors();
  }

  if (stoppedByEstop || servoTimedOut) {
    stopAllServos();
  } else if (haveServoCommand) {
    applyServoCommand(servoCommand);
  }

  if (stoppedByEstop || motorTimedOut) {
    stopAllMotors();
  } else if (haveMotorCommand) {
    applyMotorCommand(motorCommand);
  }
}
