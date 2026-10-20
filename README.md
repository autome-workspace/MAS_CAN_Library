# MAS CAN Library / Firmware

MAS のロボット向け CAN 通信の仕様（プロトコル v1）、主制御のライブラリ `mas`、基板のファームウェア。主制御（ESP32 / RP2040）から、MD とサーボの基板を 1 本の CAN でまとめて動かす。

> [!WARNING]
> 配線、書き込み、抜き差しの前に、駆動電源を切るか E-stop を掛ける。基板は電源を入れると稼働する（許可の手順は無い）。起動から 200 ms は指令に従わないが、その後は届いた指令でそのまま動く。

> [!NOTE]
> 版は `1.0.0`（2026-10-20）。旧版 v0（`MotorCAN`）とは互換が無い。v0 は [`legacy/v0/`](legacy/v0/) とタグ `v0` にある。

## 収録内容

| 場所 | 対象 | 中身 | 開発環境 |
|---|---|---|---|
| [`lib/mas/`](lib/mas/) | 主制御（ESP32、RP2040）と基板 | ライブラリ。主制御側の `Host` と、基板側のコア（`node`、`core/`） | PlatformIO / Arduino |
| [`firmware/CAN_MD/`](firmware/CAN_MD/) | STM32F042 | MD 1 ch（電流の指令と FB、duty） | CMake（Release） |
| [`firmware/CAN-Servo-v1/`](firmware/CAN-Servo-v1/) | RP2040-Zero | サーボ 8 ch ＋ MD 4 ch（duty だけ） | PlatformIO / Arduino-Pico |
| [`examples/`](examples/) | 主制御 | 使い方の例（下の表） | PlatformIO |
| [`docs/decisions/`](docs/decisions/) | — | 仕様の正本（決定の台帳） | — |

| 例 | 内容 |
|---|---|
| [`md_minimal`](examples/md_minimal/) | MD を 1 つ duty で回すだけ。まずはここから |
| [`servo_minimal`](examples/servo_minimal/) | サーボを 1 つ動かすだけ |
| [`servo_can`](examples/servo_can/) | CAN-Servo 基板のサーボと MD を 1 つずつ |
| [`md_serial_test`](examples/md_serial_test/) | シリアルから MD を操作して FB を見る試験用 |
| [`node_md_rp2040`](examples/node_md_rp2040/) | 基板側の雛形（RP2040 で 1 ch の MD を作る） |
| [`api_tour`](examples/api_tour/) | 決まった API を一通り呼ぶ見本（教える例ではない） |

## 主制御のライブラリ

### 入れ方

`lib/mas/` を自分の PlatformIO プロジェクトの `lib/mas/` に写す。このリポジトリを隣に置いて `lib_extra_dirs` で指してもよい。

```ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps = mas
```

RP2040（Arduino-Pico）では CAN に can2040 を使うので、ACAN2040 を足す（GPL-3.0 なので同梱していない）。

```ini
lib_deps =
    mas
    https://github.com/obdevel/ACAN2040.git#1.0.6
```

### 最小例

```cpp
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Md motor = can.md(0);  // ch 0 の MD

void setup() {
    Serial.begin(115200);
    if (!can.begin(5, 4)) Serial.println(mcan::toString(can.error()));  // TX, RX
    if (!can.waitReady(1000)) Serial.println("MD not ready");
}

void loop() {
    can.update();         // 送受信はすべてこの中。毎回呼ぶ
    motor.setDuty(0.3f);  // 30%。値を置くだけで、送るのは update()
}
```

覚えることは 3 つ。

- **`can.update()` を `loop()` で毎回呼ぶ。** 受信、周期送信、再起動の後始末はすべてこの中で行う。`loop()` は 100 Hz 以上で回す（下限 20 Hz）。
- **`set…` は値を置くだけ。** 指令は MD 100 Hz、サーボ 50 Hz で自動で送り直される。基板は 100 ms 指令が途絶えると止まる。
- **値は SI 単位。** 速度は rad/s、電流は A、duty は −1.0〜+1.0、角度は rad。

### 主な API

| API | 内容 |
|---|---|
| `can.begin(tx, rx)` | 始める。RP2040 は 3 つ目の引数で PIO（既定 1）。失敗の理由は `can.error()` |
| `can.update()` | 送受信。毎回呼ぶ |
| `can.waitReady(ms)` | 確保した MD がすべて応答し、準備が済むまで待つ |
| `can.estop()` / `can.clearEstop()` | E-stop を掛ける / 解く。掛けている間は 10 Hz で送り続ける |
| `can.md(ch)` → `Md` | `setDuty(d, brake)`、`setCurrent(A)`、`setSpeed(rad/s)`、`stop()` |
| | `online()`、`ready()`、`stopped()`、`feedback()`（電流、速度、位置、duty） |
| `can.servo(ch)` → `Servo` | `setAngle(rad)`、`setPulseUs(us)`、`release()`、`configCalibration(rad0, rad1, us0, us1)` |
| `can.onFault(fn)` / `can.onSettingError(fn)` | 基板の異常 / 設定の失敗を知らせる |
| `can.busLoad()` | バスの使用率（直近 1 秒、0.0〜1.0） |

API の正本は [`docs/decisions/api.md`](docs/decisions/api.md)。`Pin`、`Air`、`Solenoid`、`Controller` の取っ手はあるが、対応する基板とその線の上の形がまだ無い。

## 基板

### CAN_MD（STM32F042、1 ch）

| 項目 | 値 |
|---|---|
| ch | DIP 3 本で 0〜7（印字 1 が bit 0、on で 1）。LED が ch の数だけ点滅する（0 は消えたまま） |
| 受ける指令 | duty、電流。速度・位置はエンコーダが無いので「こなせない指令」として止まる |
| FB | 電流と duty。既定 100 Hz |
| 電流の上限 | ハードウェアの上限 15 A。**既定も 15 A**（試験のため。下の制約を参照） |
| 止め方 | 空転を出せないので、止めるときは常にブレーキ |

ビルドは CMake の **Release**（Debug は FLASH に入らない）。

```sh
cd firmware/CAN_MD
cmake --preset Release
cmake --build --preset Release
```

**新しい基板には 1 回だけ** `STM32_Programmer_CLI -c port=SWD -ob BOOT_SEL=0` を書く。書かないと、電源を素早く入れ直したときに ROM のブートローダーで止まる。

### CAN-Servo（RP2040-Zero）

| 項目 | 値 |
|---|---|
| サーボ 8 ch | J2〜J9。500〜2500 µs、50 Hz |
| MD 4 ch | J1、J10、J11、J12。duty だけ（電流・速度は測れない）。ブレーキを出せないので、止めるときは空転 |
| CAN | GP0（TX）/ GP1（RX）、PIO 1 |
| ch の先頭 | [`platformio.ini`](firmware/CAN-Servo-v1/platformio.ini) の `SERVO_FIRST_CH`（8 の倍数）、`MD_FIRST_CH`（4 の倍数）。書き込む前に変える |

```sh
cd firmware/CAN-Servo-v1
pio run -t upload
```

> [!CAUTION]
> CAN-Servo の MD と CAN_MD は同じ MD の ch の空間にある。同じ ch を受け持つ基板を 1 本の線に同時につながない（FB が衝突する）。

## プロトコルの要点

- 通常の CAN（FD ではない）、11 bit の標準 ID、1 Mbps。値は big-endian
- ID の上位 3 bit が class で、小さいほど優先度が高い（0 安全と同期、2 指令、3 FB、4 設定）
- 指令が 100 ms 途絶えた ch は止まる。E-stop はラッチし、解除か電源の再投入まで止まったまま

| ID | 内容 |
|---|---|
| `0x000` | E-stop。中身を問わず、受けた基板はすべて止まってラッチする |
| `0x001` | E-stop の解除。DLC 3、`63 6C 72`（`"clr"`） |
| `0x002` | SYNC（主制御が 20 Hz で送る。時刻合わせだけ） |
| `0x010` ＋ 種類 | 起動の知らせ |
| `0x210`〜`0x22F` | MD の指令（電流、速度、duty、位置。4 ch ずつ） |
| `0x230`〜`0x237` | サーボの指令（8 ch ずつ） |
| `0x320` ＋ ch | MD の FB |
| `0x400`〜 | 設定の要求と応答 |

詳しくは台帳: [共通](docs/decisions/common.md)、[MD](docs/decisions/md.md)、[サーボ](docs/decisions/servo.md)。ライブラリを使う限り、ID を意識する必要は無い。

## 既知の制約（1.0.x で直す）

- **CAN_MD はクロックが内蔵の HSI（16 MHz）。** 1 Mbps の CAN には精度が足りない（台帳 C-10）。SJW を広げてしのいでいる。セラロックが届いたら HSE に替える。
- **CAN_MD の電流の上限の既定が 15 A。** 小さいモーターを守らない。主制御から上限を書く API がまだ無い。
- **CAN_MD に小さな電流を指令しても、duty 0.345 より下がらない。** そのため回り続けることがある。
- 同じ線に 2 枚以上の基板をつなぐ試験は、まだ十分ではない。

残りの未決は [`docs/decisions/open.md`](docs/decisions/open.md)、MD の直す点は [`firmware/CAN_MD/TODO.md`](firmware/CAN_MD/TODO.md)。

## 配線

マイコンのピンを CAN_H / CAN_L に直接つながない。トランシーバーを介し、線の両端を 120 Ω で終端する。通信には、同じ速さで動いて ACK を返す相手が少なくとも 1 台要る。

## 開発する人へ

- PC のテスト: `make -C lib/mas/test`
- 版とブランチ: [`docs/versioning.md`](docs/versioning.md)（`main` が正式版、`next` が開発中）
- 決定は [`docs/decisions/`](docs/decisions/) の台帳にだけ書く。書き方はその [README](docs/decisions/README.md)
