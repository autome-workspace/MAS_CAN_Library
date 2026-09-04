#pragma once

#include <Arduino.h>
#include <driver/twai.h>

// ESP32 transmitter for the MAS CAN protocol. Active commands are refreshed
// by an internal FreeRTOS task every 20 ms.
class MotorCAN {
public:
    enum class DriveMode : uint8_t {
        Current,
        Speed,
        Position,
        Duty,
        ShortBrake,
    };

    static constexpr uint8_t kServoDisabled = 255;

    explicit MotorCAN(gpio_num_t txPin = GPIO_NUM_5,
                      gpio_num_t rxPin = GPIO_NUM_4);
    ~MotorCAN();

    MotorCAN(const MotorCAN &) = delete;
    MotorCAN &operator=(const MotorCAN &) = delete;

    bool begin();
    void end();
    bool ready() const;
    esp_err_t lastError() const;
    uint32_t transmittedCount() const;
    uint32_t failedCount() const;

    // Motor numbers are one-based (1..16). Values are signed int16 slots.
    bool setMotor(DriveMode mode, uint8_t motorNumber, int16_t value);
    bool setCurrent(uint8_t motorNumber, int16_t current);
    bool setSpeed(uint8_t motorNumber, int16_t speed);
    bool setPosition(uint8_t motorNumber, int16_t position);
    bool setDuty(uint8_t motorNumber, int16_t duty);
    bool setShortBrake(uint8_t motorNumber, bool enabled);
    bool releaseMotor(DriveMode mode, uint8_t motorNumber);

    // Servo channels are one-based (1..32), value is protocol data 0..200.
    bool setServo(uint8_t channel, uint8_t value);
    bool setServoPulseUs(uint8_t channel, uint16_t pulseUs);
    bool setServoAngle(uint8_t channel, float degrees);
    bool disableServo(uint8_t channel);  // periodically sends value 255
    bool releaseServo(uint8_t channel);  // stops refreshing; timeout disables

    bool setAir(uint8_t channel, bool enabled);
    bool releaseAir(uint8_t channel);

    bool emergencyStop();
    bool clearEmergencyStop();
    bool emergencyStopLatched() const;
    void releaseAll();

    bool sendRaw(uint16_t id, const uint8_t *data = nullptr, uint8_t length = 0);
    bool receive(twai_message_t &message, TickType_t timeoutTicks = 0);

private:
    static constexpr uint8_t kDriveModeCount = 5;
    static constexpr uint8_t kMotorCount = 16;
    static constexpr uint8_t kServoCount = 32;
    static constexpr uint8_t kAirCount = 32;
    static constexpr uint32_t kRefreshMs = 20;

    gpio_num_t txPin_;
    gpio_num_t rxPin_;
    volatile esp_err_t lastError_;
    volatile bool running_;
    volatile bool estopLatched_;
    volatile uint32_t transmittedCount_;
    volatile uint32_t failedCount_;
    TaskHandle_t taskHandle_;
    portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;

    int16_t motorValues_[kDriveModeCount][kMotorCount];
    uint16_t motorActive_[kDriveModeCount];
    uint8_t servoValues_[kServoCount];
    uint32_t servoActive_;
    uint8_t airValues_[kAirCount];
    uint32_t airActive_;

    static void taskEntry(void *context);
    void taskLoop();
    void serviceAlerts();
    void transmitActiveCommands();
    bool transmit(uint16_t id, const uint8_t *data, uint8_t length);

    static bool validMotor(uint8_t motorNumber);
    static bool validChannel(uint8_t channel);
    static uint8_t modeIndex(DriveMode mode);
    static uint8_t baseGroup(DriveMode mode);
    static void writeInt16BE(uint8_t *destination, int16_t value);
};
