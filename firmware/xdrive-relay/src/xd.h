// xd.h - XDrive（ODrive 3.6、fw 0.5.1 世代）の CANSimple のうち、中継に使う分だけ。
// ID ＝ node_id × 32 ＋ コマンド。値は little-endian。数値は odrivetool 0.5.1.post0 の enums.py と同じ。
#pragma once

#include <stdint.h>
#include <string.h>

namespace xd {

struct Frame {
    uint16_t id = 0;
    uint8_t dlc = 0;  // RTR のときは返してほしいバイト数
    bool rtr = false;
    uint8_t data[8] = {};
};

enum Cmd : uint8_t {
    kHeartbeat = 0x01,
    kEstop = 0x02,
    kGetMotorError = 0x03,
    kGetEncoderError = 0x04,
    kSetAxisState = 0x07,
    kGetEncoderEstimates = 0x09,
    kSetControllerModes = 0x0B,
    kSetInputVel = 0x0D,
    kSetInputTorque = 0x0E,
    kGetIq = 0x14,
    kGetVbus = 0x17,
    kClearErrors = 0x18,
};

enum class State : uint8_t { Idle = 1, ClosedLoop = 8 };
enum class Control : uint8_t { Torque = 1, Velocity = 2 };
constexpr uint8_t kInputPassthrough = 1;

// axis_error の bit
constexpr uint32_t kErrUnderVoltage = 0x2;
constexpr uint32_t kErrOverVoltage = 0x4;
constexpr uint32_t kErrMotorDisarmed = 0x20;
constexpr uint32_t kErrMotorFailed = 0x40;
constexpr uint32_t kErrEncoderFailed = 0x100;
constexpr uint32_t kErrControllerFailed = 0x200;  // controller のエラー（OVERSPEED など）。CANSimple では中身を読めない
constexpr uint32_t kErrWatchdog = 0x800;
constexpr uint32_t kErrEstop = 0x4000;
constexpr uint32_t kErrOverTemp = 0x40000;
// motor_error の bit
constexpr uint32_t kMotorDrvFault = 0x8;
constexpr uint32_t kMotorControlDeadlineMissed = 0x10;
constexpr uint32_t kMotorCurrentLimitViolation = 0x1000;
constexpr uint32_t kMotorDcBusOverCurrent = 0x8000;

inline uint16_t id(uint8_t node, uint8_t cmd) { return (uint16_t)((node << 5) | cmd); }
inline uint8_t nodeOf(uint16_t id) { return (uint8_t)(id >> 5); }
inline uint8_t cmdOf(uint16_t id) { return (uint8_t)(id & 0x1F); }

inline void putU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
inline uint32_t getU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void putF32(uint8_t* p, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    putU32(p, u);
}
inline float getF32(const uint8_t* p) {
    uint32_t u = getU32(p);
    float v;
    memcpy(&v, &u, 4);
    return v;
}

inline Frame make(uint8_t node, uint8_t cmd, uint8_t dlc) {
    Frame f;
    f.id = id(node, cmd);
    f.dlc = dlc;
    return f;
}
inline Frame request(uint8_t node, uint8_t cmd) {
    Frame f = make(node, cmd, 8);
    f.rtr = true;
    return f;
}
inline Frame setState(uint8_t node, State s) {
    Frame f = make(node, kSetAxisState, 4);
    putU32(f.data, (uint32_t)s);
    return f;
}
inline Frame setModes(uint8_t node, Control c) {
    Frame f = make(node, kSetControllerModes, 8);
    putU32(f.data, (uint32_t)c);
    putU32(f.data + 4, kInputPassthrough);
    return f;
}
inline Frame inputVel(uint8_t node, float vel) {
    Frame f = make(node, kSetInputVel, 8);
    putF32(f.data, vel);
    putF32(f.data + 4, 0.0f);
    return f;
}
inline Frame inputTorque(uint8_t node, float torque) {
    Frame f = make(node, kSetInputTorque, 4);
    putF32(f.data, torque);
    return f;
}

}  // namespace xd
