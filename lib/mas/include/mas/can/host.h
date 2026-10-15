// 主制御（API-2）。バス 1 本につき 1 つ。
//
// 時間を進めるのはアプリ: loop() で update() を呼ぶ（API-9）。受けたフレームの振り分け、
// 周期送信（MD 100 Hz、サーボ 50 Hz、SYNC 20 Hz）、再起動の後始末、コールバックはこの中。
// loop() は 100 Hz 以上を推奨、20 Hz が下限（API-10）。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device.h"
#include "device/air.h"
#include "device/controller.h"
#include "device/md.h"
#include "device/pin.h"
#include "device/servo.h"
#include "device/solenoid.h"
#include "port.h"

namespace mas {
namespace can {

// 主制御全体の異常（API-47）。文字列は toString で得る。
enum class Error : uint8_t {
    None = 0,
    PortStartFailed, // CAN の開始に失敗した
    OutOfMemory,     // 取っ手の中身を確保できなかった
    BusLoadHigh,     // バスの使用率が configBusLoadWarning を超えた
    CommandConflict, // 同じ束に別の種類の指令の ch がいる（API-52）
};
const char* toString(Error e);

namespace detail {

enum class ReqTag : uint8_t { CurrentLsb, SpeedLsb, ReadFault, ClearFault };

struct SettingReq {
    DeviceState* dev = nullptr;
    uint8_t op = 0, item = 0;
    uint8_t value[4] = {0, 0, 0, 0};
    ReqTag tag = ReqTag::CurrentLsb;
};

} // namespace detail

class Host {
  public:
    using FaultHandler = void (*)(Device& dev, uint32_t new_bits);
    using SettingErrorHandler = void (*)(Device& dev, uint8_t result); // 来なければ 255

    constexpr Host() = default; // どのグローバルより先にできる（API-35）
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // ---- 初期化（API-7、API-8 → API-47） ----
    bool begin(Port& port);
#if defined(ESP_PLATFORM)
    bool begin(int tx, int rx); // ESP32 の近道（tx, rx の順）
#endif

    // ---- 取っ手（API-5。初めて求めたときに中身を確保する） ----
    Md md(uint8_t ch);
    Controller controller(uint8_t ch);
    Servo servo(uint8_t ch);
    Pin pin(uint8_t ch);
    Air air(uint8_t ch);
    Solenoid solenoid(uint8_t ch);

    // ---- 送受信 ----
    void update();
    void flush(); // 値が変わった指令の束を今すぐ送る（API-36）
    bool waitReady(uint32_t timeout_ms); // 全 ch が online かつ ready になるまで待つ（API-45）

    // ---- E-stop（API-21〜24、API-48） ----
    void estop();
    void clearEstop();
    bool estopped() const { return estop_own_ || estop_seen_; }

    // ---- 異常 ----
    void onFault(FaultHandler handler) { fault_handler_ = handler; }
    void onSettingError(SettingErrorHandler handler) { setting_error_handler_ = handler; }
    Error error() const { return error_; }

    // ---- 監視 ----
    float busLoad() const { return bus_load_; }           // 直近 1 秒の使用率（API-37）
    void configBusLoadWarning(float ratio) { load_warn_ = ratio; } // API-38
    uint32_t rxOverflows() const { return port_ ? port_->rxOverflows() : 0; }

  private:
    friend class Md;
    friend class Controller;
    friend class Servo;

    detail::DeviceState* find(Kind kind, uint8_t ch) const;
    detail::DeviceState* obtain(Kind kind, uint8_t ch);
    bool send(const Frame& f);
    void countBits(const Frame& f);
    void receiveAll(uint32_t now);
    void handleMdFeedback(const Frame& f, uint32_t now);
    void handleController(const Frame& f, uint32_t now);
    void handleSettingResponse(const Frame& f);
    void onMdRestart(detail::DeviceState* s);
    void queueResolutions(detail::DeviceState* s, bool front);
    void popSetting();
    bool queueSetting(const detail::SettingReq& req, bool front);
    void serviceSettings(uint32_t now);
    void serviceEstop(uint32_t now);
    void sendMd(bool only_dirty, uint32_t now);
    void sendServos(bool only_dirty, uint32_t now);
    void resetCommands();
    void updateBusLoad(uint32_t now);

    Port* port_ = nullptr;
    detail::DeviceState* head_ = nullptr;
    FaultHandler fault_handler_ = nullptr;
    SettingErrorHandler setting_error_handler_ = nullptr;
    Error error_ = Error::None;
    bool started_ = false;

    uint32_t md_sent_us_[8] = {};     // 束ごとに最後に送った時刻（flush も数え直す）
    uint32_t servo_sent_us_[8] = {};
    uint32_t last_sync_us_ = 0;

    bool estop_own_ = false, estop_seen_ = false;
    uint32_t last_estop_us_ = 0;
    uint8_t clear_retries_ = 0;
    bool clear_checking_ = false;
    uint32_t clear_check_us_ = 0;

    static constexpr uint8_t kQueue = 16;
    detail::SettingReq queue_[kQueue] = {};
    uint8_t q_len_ = 0; // queue_[0] が先頭
    bool req_active_ = false;
    uint8_t req_seq_ = 0, req_retries_ = 0;
    uint32_t req_sent_us_ = 0;

    uint32_t load_bits_ = 0, load_start_us_ = 0;
    float bus_load_ = 0, load_warn_ = 0;
};

} // namespace can
} // namespace mas
