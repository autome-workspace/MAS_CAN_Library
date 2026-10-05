// CAN のバス 1 本（A1、A3、A8）。
//
// 時間を進めるのはアプリ: loop() で update() を呼ぶ。update() の中で、受けたフレームの
// 振り分け、期限の来た周期送信（指令 100 Hz、SYNC 20 Hz）、再起動の後始末、
// コールバックの呼び出しを行う。loop() は少なくとも約 20 Hz 以上で回すこと。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "md.h"
#include "port.h"

namespace mas {
namespace can {

namespace detail {

enum class MdMode : uint8_t { None, Current, Speed, Duty, Position };

struct MdState {
    // 指令
    MdMode mode = MdMode::None;
    bool raw = false;       // 生の値で指令している
    float value = 0;        // SI（A、rad/s、duty の割合）
    bool brake = false;     // duty のときの減衰モード
    int16_t raw_value = 0;  // raw が true のとき
    bool conflict = false;  // 同じフレームに別の種類の指令の ch がいて送れない

    // 保存しない設定（A4）
    bool current_lsb_set = false, speed_lsb_set = false;
    float current_lsb_ma = 1.0f, speed_lsb_rpm = 1.0f;
    bool current_lsb_pending = false, speed_lsb_pending = false;

    // FB
    Md::Feedback fb;
    bool fb_seen = false;
    uint32_t last_raw_pos = 0;
    bool relink = false;       // 次の FB で位置を結び直す（基板の再起動の後）
    bool fault_known = false;  // 今立っている異常の詳細を読んだ
    bool fault_reading = false;
    uint32_t fault_detail = 0;
    uint8_t clear_retries = 0;
};

struct ControllerState {
    int8_t axis[4] = {0, 0, 0, 0};
    uint8_t trigger[2] = {0, 0};
    uint16_t buttons = 0;
    uint16_t pressed = 0, released = 0;
    bool link = false;
    uint32_t last_us = 0;
    bool seen = false;
    float deadzone = 0;
};

enum class ReqTag : uint8_t { User, CurrentLsb, SpeedLsb, ReadFault, ClearFault };

struct SettingReq {
    uint8_t kind, ch, op, item;
    uint8_t value[4];
    ReqTag tag;
};

}  // namespace detail

class Bus {
  public:
    static constexpr uint8_t kMdCount = 32;
    static constexpr uint8_t kControllerCount = 8;

    // 異常の知らせ（A7）。update() の中から、新しく立った bit だけを渡して呼ばれる。
    using FaultHandler = void (*)(Md &md, uint32_t new_bits);
    // 設定の書き込みの失敗（A4）。result は結果の番号（応答が来なければ 255）。
    using SettingErrorHandler = void (*)(Md &md, uint8_t item, uint8_t result);

    // ポートを渡して始める。
    void begin(Port &port);

    // 送受信を進める（A3）。
    void update();

    // 相手の取っ手（A1）。
    Md md(uint8_t ch) { return Md(this, ch); }
    Controller controller(uint8_t ch) { return Controller(this, ch); }

    // 指定した MD がすべて online になるまで待つ（中で update() を回す）。間に合えば true。
    bool waitOnline(const Md *mds, size_t count, uint32_t timeout_ms);

    // ---- E-stop（A8） ----
    void estop();           // clearEstop() まで 10 Hz で送り続ける。その間の指令は止める値
    void clearEstop();      // 指令を止める値に戻してから解除を送る
    bool estopped() const;  // 自分の E-stop か、ほかの送り手の E-stop を受けた状態か

    void onFault(FaultHandler handler);
    void onSettingError(SettingErrorHandler handler);

    // SYNC を送るか（既定は送る）。主制御が複数いるときは、時刻の親の 1 台だけが送る。
    void setSyncSource(bool enable);

  private:
    friend class Md;
    friend class Controller;

    void receiveAll(uint32_t now);
    void handleMdFeedback(const Frame &f, uint32_t now);
    void handleController(const Frame &f, uint32_t now);
    void handleSettingResponse(const Frame &f);
    void onBoardRestart(uint8_t ch);
    void sendCommands();
    void serviceSettings(uint32_t now);
    void serviceEstop(uint32_t now);
    bool queueSetting(const detail::SettingReq &req);
    void queueLsbWrites(uint8_t ch);
    int16_t rawCommand(uint8_t ch) const;
    bool chReady(uint8_t ch) const;

    Port *port_ = nullptr;
    FaultHandler fault_handler_ = nullptr;
    SettingErrorHandler setting_error_handler_ = nullptr;
    bool sync_source_ = true;

    detail::MdState md_[kMdCount];
    detail::ControllerState pad_[kControllerCount];

    uint32_t last_cmd_us_ = 0, last_sync_us_ = 0;
    bool started_ = false;

    bool estop_own_ = false, estop_seen_ = false;
    uint32_t last_estop_us_ = 0;
    uint8_t clear_retries_ = 0;
    uint32_t clear_check_us_ = 0;
    bool clear_checking_ = false;

    static constexpr uint8_t kQueue = 16;
    detail::SettingReq queue_[kQueue];
    uint8_t q_head_ = 0, q_len_ = 0;
    bool req_active_ = false;
    uint8_t req_seq_ = 0, req_retries_ = 0;
    uint32_t req_deadline_us_ = 0;
};

}  // namespace can
}  // namespace mas
