#pragma once

#include <Arduino.h>
#include <driver/twai.h>

class MotorCAN {
public:
    MotorCAN(gpio_num_t txPin, gpio_num_t rxPin);

    bool begin();
    esp_err_t lastError() const;

    // motorNumber is the protocol's one-based n (1..16).
    bool setCurrent(uint8_t motorNumber, int16_t currentMilliAmps);

    // Sends the latched short-brake command defined by CAN_protocol.md.
    // enabled=true applies the brake; false releases it to coast.
    bool setShortBrake(uint8_t motorNumber, bool enabled);

private:
    gpio_num_t txPin_;
    gpio_num_t rxPin_;
    esp_err_t lastError_;

    bool transmit(uint16_t id, const uint8_t *data, uint8_t length);
    bool sendSlotCommand(uint8_t motorNumber, uint8_t identityGroup,
                         int16_t value);

    static uint8_t groupFor(uint8_t identityGroup, uint8_t motorNumber);
    static uint8_t nodeFor(uint8_t motorNumber);
    static uint16_t commandId(uint8_t identityGroup, uint8_t motorNumber);
    static void writeInt16BE(uint8_t *destination, int16_t value);
};
