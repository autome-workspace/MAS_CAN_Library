// CAN の送受信と時刻の窓口（ポート）。
//
// ライブラリはハードウェアに依存しない（A3）。CAN のコントローラと時計は、このクラスを
// 継承したポートが受け持つ。ESP32 なら TWAI のポート（esp32_twai.h）を使う。
#pragma once

#include <stdint.h>

#include "core/frame.h"

namespace mas {
namespace can {

using Frame = mas_can_frame_t;

class Port {
  public:
    virtual ~Port() = default;

    // 1 本送る。待たずに戻る。送信の待ち行列が満杯なら false。
    virtual bool send(const Frame &frame) = 0;

    // 受けたフレームを 1 本取り出す。待たずに戻る。無ければ false。
    virtual bool receive(Frame &frame) = 0;

    // 今の時刻（µs。一周してよい）。SYNC の中身と、周期や途絶の計算に使う。
    virtual uint32_t micros() = 0;
};

}  // namespace can
}  // namespace mas
