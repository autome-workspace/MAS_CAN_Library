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

class Bus {
 public:
  // 異常の知らせ（A7）。update() の中から、新しく立った bit だけを渡して呼ばれる。
  using FaultHandler = void (*)(Md &md, uint32_t new_bits);

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
  void estop();             // clearEstop() まで 10 Hz で送り続ける。その間の指令は止める値
  void clearEstop();        // 指令を止める値に戻してから解除を送る
  bool estopped() const;    // 自分の E-stop か、ほかの送り手の E-stop を受けた状態か

  void onFault(FaultHandler handler);

  // SYNC を送るか（既定は送る）。主制御が複数いるときは、時刻の親の 1 台だけが送る。
  void setSyncSource(bool enable);

 private:
  friend class Md;
  friend class Controller;
  Port *port_ = nullptr;
  FaultHandler fault_handler_ = nullptr;
  bool sync_source_ = true;
};

}  // namespace can
}  // namespace mas
