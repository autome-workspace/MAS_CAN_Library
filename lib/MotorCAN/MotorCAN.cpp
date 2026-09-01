#include "MotorCAN.h"

#include <cstring>

namespace {
constexpr uint8_t kFirstMotorNumber = 1;
constexpr uint8_t kLastMotorNumber = 16;
constexpr uint8_t kCurrentIdentityGroup = 0;
constexpr uint8_t kShortBrakeIdentityGroup = 12;
constexpr uint16_t kClass2BaseId = 0x200;
constexpr TickType_t kTransmitTimeout = pdMS_TO_TICKS(10);
}  // namespace

MotorCAN::MotorCAN(gpio_num_t txPin, gpio_num_t rxPin)
    : txPin_(txPin), rxPin_(rxPin), lastError_(ESP_OK) {}

bool MotorCAN::begin() {
    twai_general_config_t generalConfig =
        TWAI_GENERAL_CONFIG_DEFAULT(txPin_, rxPin_, TWAI_MODE_NORMAL);
    twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t filterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    lastError_ =
        twai_driver_install(&generalConfig, &timingConfig, &filterConfig);
    if (lastError_ != ESP_OK) {
        return false;
    }

    lastError_ = twai_start();
    if (lastError_ != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }

    return true;
}

esp_err_t MotorCAN::lastError() const { return lastError_; }

bool MotorCAN::setCurrent(uint8_t motorNumber, int16_t currentMilliAmps) {
    return sendSlotCommand(motorNumber, kCurrentIdentityGroup,
                           currentMilliAmps);
}

bool MotorCAN::setShortBrake(uint8_t motorNumber, bool enabled) {
    return sendSlotCommand(motorNumber, kShortBrakeIdentityGroup,
                           enabled ? 1 : 0);
}

bool MotorCAN::sendSlotCommand(uint8_t motorNumber, uint8_t identityGroup,
                               int16_t value) {
    if (motorNumber < kFirstMotorNumber || motorNumber > kLastMotorNumber) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }

    uint8_t data[8] = {};
    const uint8_t node = nodeFor(motorNumber);
    writeInt16BE(&data[2 * (node - 1)], value);

    // The DLC must include the addressed node's complete two-byte slot.
    return transmit(commandId(identityGroup, motorNumber), data, 2 * node);
}

bool MotorCAN::transmit(uint16_t id, const uint8_t *data, uint8_t length) {
    if (id > 0x7FF || length > 8 || (length > 0 && data == nullptr)) {
        lastError_ = ESP_ERR_INVALID_ARG;
        return false;
    }

    twai_message_t message = {};
    message.identifier = id;
    message.data_length_code = length;
    if (length > 0) {
        std::memcpy(message.data, data, length);
    }

    lastError_ = twai_transmit(&message, kTransmitTimeout);
    return lastError_ == ESP_OK;
}

uint8_t MotorCAN::groupFor(uint8_t identityGroup, uint8_t motorNumber) {
    return identityGroup + (motorNumber - 1) / 4;
}

uint8_t MotorCAN::nodeFor(uint8_t motorNumber) {
    return (motorNumber - 1) % 4 + 1;
}

uint16_t MotorCAN::commandId(uint8_t identityGroup, uint8_t motorNumber) {
    return kClass2BaseId | (groupFor(identityGroup, motorNumber) << 3);
}

void MotorCAN::writeInt16BE(uint8_t *destination, int16_t value) {
    const uint16_t raw = static_cast<uint16_t>(value);
    destination[0] = static_cast<uint8_t>(raw >> 8);
    destination[1] = static_cast<uint8_t>(raw);
}
