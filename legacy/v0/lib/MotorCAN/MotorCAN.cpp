#include "MotorCAN.h"

#include <cstring>

namespace {
constexpr uint16_t kClass2BaseId = 0x200;
constexpr TickType_t kTransmitTimeout = pdMS_TO_TICKS(10);
constexpr uint8_t kMotorGroupSize = 4;
constexpr uint8_t kByteGroupSize = 8;
}  // namespace

MotorCAN::MotorCAN(gpio_num_t txPin, gpio_num_t rxPin)
    : txPin_(txPin), rxPin_(rxPin), lastError_(ESP_OK), running_(false),
      estopLatched_(false), transmittedCount_(0), failedCount_(0),
      taskHandle_(nullptr), motorValues_{}, motorActive_{}, servoValues_{},
      servoActive_(0), airValues_{}, airActive_(0) {
    std::memset(servoValues_, kServoDisabled, sizeof(servoValues_));
}

MotorCAN::~MotorCAN() { end(); }

bool MotorCAN::begin() {
    if (running_) return true;

    twai_general_config_t general =
        TWAI_GENERAL_CONFIG_DEFAULT(txPin_, rxPin_, TWAI_MODE_NORMAL);
    general.tx_queue_len = 16;
    general.rx_queue_len = 32;
    const twai_timing_config_t timing = TWAI_TIMING_CONFIG_1MBITS();
    const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    lastError_ = twai_driver_install(&general, &timing, &filter);
    if (lastError_ != ESP_OK) return false;
    lastError_ = twai_start();
    if (lastError_ != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }

    uint32_t configuredAlerts = 0;
    twai_reconfigure_alerts(TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                                TWAI_ALERT_TX_FAILED,
                            &configuredAlerts);
    running_ = true;
    if (xTaskCreate(taskEntry, "mas-can", 4096, this, 2, &taskHandle_) !=
        pdPASS) {
        running_ = false;
        lastError_ = ESP_ERR_NO_MEM;
        twai_stop();
        twai_driver_uninstall();
        taskHandle_ = nullptr;
        return false;
    }
    return true;
}

void MotorCAN::end() {
    if (!running_ && taskHandle_ == nullptr) return;
    running_ = false;
    if (taskHandle_ != nullptr) {
        vTaskDelete(taskHandle_);
        taskHandle_ = nullptr;
    }
    twai_stop();
    twai_driver_uninstall();
}

bool MotorCAN::ready() const { return running_; }
esp_err_t MotorCAN::lastError() const { return lastError_; }
uint32_t MotorCAN::transmittedCount() const { return transmittedCount_; }
uint32_t MotorCAN::failedCount() const { return failedCount_; }

bool MotorCAN::setMotor(DriveMode mode, uint8_t motorNumber, int16_t value) {
    if (!validMotor(motorNumber)) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    if (estopLatched_) {
        lastError_ = ESP_ERR_INVALID_STATE;
        return false;
    }
    const uint8_t index = modeIndex(mode);
    const uint8_t slot = motorNumber - 1;
    portENTER_CRITICAL(&stateMux_);
    // A board that supports multiple drive modes must only receive one of
    // them from this controller. The newly selected mode wins.
    for (uint8_t other = 0; other < kDriveModeCount; ++other) {
        motorActive_[other] &= static_cast<uint16_t>(~(1U << slot));
    }
    motorValues_[index][slot] = value;
    motorActive_[index] |= static_cast<uint16_t>(1U << slot);
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

bool MotorCAN::setCurrent(uint8_t motorNumber, int16_t current) {
    return setMotor(DriveMode::Current, motorNumber, current);
}
bool MotorCAN::setSpeed(uint8_t motorNumber, int16_t speed) {
    return setMotor(DriveMode::Speed, motorNumber, speed);
}
bool MotorCAN::setPosition(uint8_t motorNumber, int16_t position) {
    return setMotor(DriveMode::Position, motorNumber, position);
}
bool MotorCAN::setDuty(uint8_t motorNumber, int16_t duty) {
    return setMotor(DriveMode::Duty, motorNumber, duty);
}
bool MotorCAN::setShortBrake(uint8_t motorNumber, bool enabled) {
    return setMotor(DriveMode::ShortBrake, motorNumber, enabled ? 1 : 0);
}

bool MotorCAN::releaseMotor(DriveMode mode, uint8_t motorNumber) {
    if (!validMotor(motorNumber)) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    const uint8_t index = modeIndex(mode);
    const uint8_t slot = motorNumber - 1;
    portENTER_CRITICAL(&stateMux_);
    motorActive_[index] &= static_cast<uint16_t>(~(1U << slot));
    motorValues_[index][slot] = 0;
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

bool MotorCAN::setServo(uint8_t channel, uint8_t value) {
    if (!validChannel(channel) || value > 200) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    if (estopLatched_) {
        lastError_ = ESP_ERR_INVALID_STATE;
        return false;
    }
    const uint8_t slot = channel - 1;
    portENTER_CRITICAL(&stateMux_);
    servoValues_[slot] = value;
    servoActive_ |= 1UL << slot;
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

bool MotorCAN::setServoPulseUs(uint8_t channel, uint16_t pulseUs) {
    if (pulseUs < 500 || pulseUs > 2500) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    return setServo(channel,
                    static_cast<uint8_t>((pulseUs - 500U + 5U) / 10U));
}

bool MotorCAN::setServoAngle(uint8_t channel, float degrees) {
    if (degrees < 0.0f || degrees > 180.0f) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    return setServo(channel, static_cast<uint8_t>(
                                 degrees * 200.0f / 180.0f + 0.5f));
}

bool MotorCAN::disableServo(uint8_t channel) {
    if (!validChannel(channel)) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    const uint8_t slot = channel - 1;
    portENTER_CRITICAL(&stateMux_);
    servoValues_[slot] = kServoDisabled;
    servoActive_ |= 1UL << slot;
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

bool MotorCAN::releaseServo(uint8_t channel) {
    if (!validChannel(channel)) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    const uint8_t slot = channel - 1;
    portENTER_CRITICAL(&stateMux_);
    servoActive_ &= ~(1UL << slot);
    servoValues_[slot] = kServoDisabled;
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

bool MotorCAN::setAir(uint8_t channel, bool enabled) {
    if (!validChannel(channel)) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    if (estopLatched_) {
        lastError_ = ESP_ERR_INVALID_STATE;
        return false;
    }
    const uint8_t slot = channel - 1;
    portENTER_CRITICAL(&stateMux_);
    airValues_[slot] = enabled ? 1 : 0;
    airActive_ |= 1UL << slot;
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

bool MotorCAN::releaseAir(uint8_t channel) {
    if (!validChannel(channel)) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }
    const uint8_t slot = channel - 1;
    portENTER_CRITICAL(&stateMux_);
    airActive_ &= ~(1UL << slot);
    airValues_[slot] = 0;
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

bool MotorCAN::emergencyStop() {
    // Stop the refresh task before placing E-stop in the TX queue. Keep the
    // local latch set even when the physical transmission fails: resuming
    // output without an explicit clear would be unsafe.
    estopLatched_ = true;
    releaseAll();
    const bool sent = transmit(0x000, nullptr, 0);
    return sent;
}

bool MotorCAN::clearEmergencyStop() {
    const uint8_t signature[] = {'c', 'l', 'r'};
    const bool sent = transmit(0x001, signature, sizeof(signature));
    if (sent) estopLatched_ = false;
    return sent;
}

bool MotorCAN::emergencyStopLatched() const { return estopLatched_; }

void MotorCAN::releaseAll() {
    portENTER_CRITICAL(&stateMux_);
    std::memset(motorValues_, 0, sizeof(motorValues_));
    std::memset(motorActive_, 0, sizeof(motorActive_));
    std::memset(servoValues_, kServoDisabled, sizeof(servoValues_));
    servoActive_ = 0;
    std::memset(airValues_, 0, sizeof(airValues_));
    airActive_ = 0;
    portEXIT_CRITICAL(&stateMux_);
}

bool MotorCAN::sendRaw(uint16_t id, const uint8_t *data, uint8_t length) {
    return transmit(id, data, length);
}

bool MotorCAN::receive(twai_message_t &message, TickType_t timeoutTicks) {
    if (!running_) {
        lastError_ = ESP_ERR_INVALID_STATE;
        return false;
    }
    lastError_ = twai_receive(&message, timeoutTicks);
    return lastError_ == ESP_OK;
}

void MotorCAN::taskEntry(void *context) {
    static_cast<MotorCAN *>(context)->taskLoop();
}

void MotorCAN::taskLoop() {
    TickType_t wakeAt = xTaskGetTickCount();
    while (running_) {
        serviceAlerts();
        if (!estopLatched_) transmitActiveCommands();
        vTaskDelayUntil(&wakeAt, pdMS_TO_TICKS(kRefreshMs));
    }
    taskHandle_ = nullptr;
    vTaskDelete(nullptr);
}

void MotorCAN::serviceAlerts() {
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) != ESP_OK || alerts == 0) return;
    if (alerts & TWAI_ALERT_TX_FAILED) ++failedCount_;
    if (alerts & TWAI_ALERT_BUS_OFF) lastError_ = twai_initiate_recovery();
    if (alerts & TWAI_ALERT_BUS_RECOVERED) lastError_ = twai_start();
}

void MotorCAN::transmitActiveCommands() {
    int16_t motorValues[kDriveModeCount][kMotorCount];
    uint16_t motorActive[kDriveModeCount];
    uint8_t servoValues[kServoCount];
    uint32_t servoActive;
    uint8_t airValues[kAirCount];
    uint32_t airActive;

    portENTER_CRITICAL(&stateMux_);
    std::memcpy(motorValues, motorValues_, sizeof(motorValues));
    std::memcpy(motorActive, motorActive_, sizeof(motorActive));
    std::memcpy(servoValues, servoValues_, sizeof(servoValues));
    servoActive = servoActive_;
    std::memcpy(airValues, airValues_, sizeof(airValues));
    airActive = airActive_;
    portEXIT_CRITICAL(&stateMux_);

    for (uint8_t mode = 0; mode < kDriveModeCount; ++mode) {
        for (uint8_t group = 0; group < 4; ++group) {
            const uint16_t mask = static_cast<uint16_t>(0x0FU << (group * 4));
            if ((motorActive[mode] & mask) == 0) continue;
            uint8_t data[8] = {};
            for (uint8_t slot = 0; slot < kMotorGroupSize; ++slot) {
                const uint8_t index = group * kMotorGroupSize + slot;
                writeInt16BE(&data[slot * 2], motorValues[mode][index]);
            }
            const uint8_t protocolGroup =
                baseGroup(static_cast<DriveMode>(mode)) + group;
            transmit(kClass2BaseId |
                         (static_cast<uint16_t>(protocolGroup) << 3),
                     data, sizeof(data));
        }
    }

    for (uint8_t group = 0; group < 4; ++group) {
        const uint32_t mask = 0xFFUL << (group * 8);
        if (servoActive & mask) {
            uint8_t data[8];
            for (uint8_t slot = 0; slot < kByteGroupSize; ++slot) {
                const uint8_t index = group * kByteGroupSize + slot;
                data[slot] = (servoActive & (1UL << index))
                                 ? servoValues[index]
                                 : kServoDisabled;
            }
            const uint8_t protocolGroup = 20 + group;
            transmit(kClass2BaseId |
                         (static_cast<uint16_t>(protocolGroup) << 3),
                     data, sizeof(data));
        }
        if (airActive & mask) {
            uint8_t data[8] = {};
            for (uint8_t slot = 0; slot < kByteGroupSize; ++slot) {
                const uint8_t index = group * kByteGroupSize + slot;
                if (airActive & (1UL << index)) data[slot] = airValues[index];
            }
            const uint8_t protocolGroup = 24 + group;
            transmit(kClass2BaseId |
                         (static_cast<uint16_t>(protocolGroup) << 3),
                     data, sizeof(data));
        }
    }
}

bool MotorCAN::transmit(uint16_t id, const uint8_t *data, uint8_t length) {
    if (!running_ || id > 0x7FF || length > 8 ||
        (length > 0 && data == nullptr)) {
        lastError_ = !running_ ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
        ++failedCount_;
        return false;
    }
    twai_message_t message = {};
    message.identifier = id;
    message.data_length_code = length;
    if (length > 0) std::memcpy(message.data, data, length);
    lastError_ = twai_transmit(&message, kTransmitTimeout);
    if (lastError_ == ESP_OK) {
        ++transmittedCount_;
        return true;
    }
    ++failedCount_;
    return false;
}

bool MotorCAN::validMotor(uint8_t motorNumber) {
    return motorNumber >= 1 && motorNumber <= kMotorCount;
}
bool MotorCAN::validChannel(uint8_t channel) {
    return channel >= 1 && channel <= kServoCount;
}
uint8_t MotorCAN::modeIndex(DriveMode mode) {
    return static_cast<uint8_t>(mode);
}
uint8_t MotorCAN::baseGroup(DriveMode mode) {
    switch (mode) {
        case DriveMode::Current: return 0;
        case DriveMode::Speed: return 4;
        case DriveMode::Position: return 8;
        case DriveMode::Duty: return 12;
        case DriveMode::ShortBrake: return 16;
    }
    return 0;
}
void MotorCAN::writeInt16BE(uint8_t *destination, int16_t value) {
    const uint16_t raw = static_cast<uint16_t>(value);
    destination[0] = static_cast<uint8_t>(raw >> 8);
    destination[1] = static_cast<uint8_t>(raw);
}
