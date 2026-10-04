// オムニ機体（エンコーダ付きの MD × 4、X 字配置）を、コントローラで手動操作する例。
//
// これは「こう書けたら嬉しい」という使う側のたたき台で、ライブラリはまだ無い。
// 【案】の印は、ライブラリの API の候補。ここで書き心地を確かめて決める。
//
// 配線: ESP32-S3 ── ATA6561 ── CAN バス（MD × 4、コントローラの受信基板）

#include <Arduino.h>
#include <mascan.h>  // 【案】新しいライブラリの入口。名前空間は mas

// ---- バスとつながる相手 -------------------------------------------------

// CAN のバス 1 本につき 1 つ（A1。I2C や SPI と分けるため CanBus）。TWAI の初期化、
// SYNC（20 Hz）と指令（100 Hz）の周期送信、受信の振り分け、レート制限を受け持つ（update() の中で）。
mas::CanBus can;

// 相手は「種類と ch」で指す（A1）。基板の枚数や DIP は知らなくてよい。
// can.md(ch) は軽い取っ手（handle）を返し、中身（指令の値、FB）はバスが持つ。
mas::Md wheel[4] = {can.md(0), can.md(1), can.md(2), can.md(3)};
mas::Controller pad = can.controller(0);

// ---- 機体の寸法 ---------------------------------------------------------

constexpr float kWheelRadius_m = 0.05f;  // 車輪の半径
constexpr float kBodyRadius_m = 0.25f;   // 機体の中心から車輪までの距離
constexpr float kMaxSpeed_mps = 2.0f;    // スティックを倒しきったときの速さ
constexpr float kMaxTurn_radps = 3.0f;   // スティックを倒しきったときの旋回の速さ
// 車輪の向き（X 字。前から反時計回りに 45°、135°、225°、315°）
constexpr float kWheelAngle_rad[4] = {PI / 4, 3 * PI / 4, 5 * PI / 4, 7 * PI / 4};

// 車体の速度（m/s、rad/s）から、各車輪の出力軸の角速度（rad/s）を計算する（逆運動学）。
// ここは機体ごとの計算なので、ライブラリには入れない（判断は主制御が持つ）。
static void omniInverse(float vx, float vy, float omega, float w_out[4]) {
  for (int i = 0; i < 4; i++) {
    float v = -sinf(kWheelAngle_rad[i]) * vx + cosf(kWheelAngle_rad[i]) * vy +
              kBodyRadius_m * omega;  // 車輪の接線方向の速さ（m/s）
    w_out[i] = v / kWheelRadius_m;  // 出力軸の角速度（rad/s）
  }
}

void setup() {
  Serial.begin(115200);

  // 【案】ピンを渡して始める。以後、update() のたびに SYNC と指令を送る。
  // ATA6561 の STBY を MCU でつないでいる場合は、そのピンも渡す（無ければ省く）。
  can.begin({.tx = GPIO_NUM_5, .rx = GPIO_NUM_4});

  for (auto &w : wheel) {
    // 【案】保存しない設定（主制御の「選択」）は、ここで書く。
    // ライブラリが覚えておき、基板の再起動（起動の知らせ）を見たら自動で送り直す。
    // 足回りは遅いので、速度の分解能を細かくする（0.01 rad/s/LSB ≈ 0.095 rpm）。
    w.setSpeedResolution(0.01f);  // rad/s/LSB（設定 49。ライブラリが rpm/LSB に換算して書く）

    // 【案】保存する設定（減速比など「事実」）は、普段はスケッチに書かない。
    // 基板を組んだときに、PC のツールなどで一度だけ書いて保存しておく。
    // スケッチでは「想定どおりか」を確かめるだけにできる。
    // w.expectGearRatio(19.2032f);  // 違ったら起動時に知らせる、など
  }

  // 【案】基板がそろうのを待つ（FB が届いたか）。手動の機体なので、そろわなくても進む。
  if (!can.waitOnline(wheel, 4, /*timeout_ms=*/1000)) {
    Serial.println("見つからない MD がある");
  }
}

void loop() {
  // 送受信を進める（A3）。受けたフレームの振り分け、期限の来た周期送信（指令、SYNC）、
  // 基板の再起動の後始末をここで行う。loop() は少なくとも約 20 Hz 以上で回すこと。
  can.update();

  // ---- 非常停止 ----
  // コントローラには非常停止のボタンが無いので、ここでは L1 と R1 の同時押しにする。
  // 【案】estop() は E-stop を送る。clearEstop() は、指令を止める値に戻してから解除を送る（D21）。
  if (pad.held(mas::Button::L1) && pad.held(mas::Button::R1)) {
    can.estop();
  }
  if (can.estopped() && pad.pressed(mas::Button::Up)) {
    can.clearEstop();
  }

  // ---- 操作 ----
  float w[4] = {0, 0, 0, 0};  // 各車輪の目標（rad/s）
  // 【案】無線が切れていれば connected() が false。スティックの値は 0 になっている。
  if (pad.connected()) {
    float vx = pad.leftY() * kMaxSpeed_mps;   // 前後（-1.0〜+1.0 に直した値）
    float vy = -pad.leftX() * kMaxSpeed_mps;  // 左右（左が正）
    float omega = -pad.rightX() * kMaxTurn_radps;
    omniInverse(vx, vy, omega, w);
  }

  for (int i = 0; i < 4; i++) {
    // 指令は SI で渡す（A2）。LSB への換算と周期送信はライブラリの仕事。
    // 同じ値を毎回呼んでもよいし、変わったときだけ呼んでもよい。
    wheel[i].setSpeed(w[i]);
  }

  // ---- 様子を見る（10 Hz で表示） ----
  static uint32_t last_print = 0;
  if (millis() - last_print >= 100) {
    last_print = millis();
    for (int i = 0; i < 4; i++) {
      auto &w = wheel[i];
      // FB は最後に届いた値。SI で返す（A2）。
      // position() は折り返しを積み上げたカウント（int64、1 回転 ＝ 65536、電源投入時が 0）。
      Serial.printf("[%d] %s %7.2f rad/s %6.2f A %10lld cnt%s%s\n", i,
                    w.online() ? "ok" : "--",  // 【案】FB が途絶えていないか
                    w.speed(), w.current(), (long long)w.position(),
                    w.stopped() ? " 停止中" : "",       // FB の bit 4
                    w.hasFault() ? " 異常あり" : "");   // FB の bit 0
    }
  }
}

// ---- ここから下は、必要な人だけ使う機能の例 ----

// 【案】異常の詳細を知りたいとき（FB の bit 0 が立ったら、ライブラリが自動で読みに行く）。
// 再起動（bit 0）の後始末（設定の送り直し、bit を消す）はライブラリが済ませるので、
// ここに届くのはそれ以外の異常だけ。
// can.onFault([](mas::Md &md, uint32_t detail) {
//   Serial.printf("MD ch %d: 異常 0x%08lx\n", md.ch(), detail);
// });
