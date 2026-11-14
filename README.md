# MAS CAN

MAS のロボットで、主制御のマイコンから、モータードライバーやサーボの基板を動かすための決まりと、そのライブラリです。主制御と基板を 2 本の線（CAN）でつなぎ、主制御のプログラムは、基板の番号を指定して動かします。

```cpp
mcan::Md motor = can.md(0);   // モータードライバーの 0 番

void loop() {
    can.update();
    motor.setDuty(0.3f);      // 30% の強さで回す
}
```

## 今の状態

版は v1.0.0 です。モータードライバーとサーボが動きます。主制御は ESP32（ESP32-S3 を含む）と Raspberry Pi Pico（RP2040）に対応しています。

相手にできるのは、この決まりを話すファームが入った基板だけです。今あるのは部の自作のもの 4 種類で、一覧は [今ある基板](docs/guide/boards.md) にあります。市販のモータードライバーを、そのままつなぐことはできません。実機で確かめていないことがある文書には、その旨が書いてあります。

## どこを読むか

| あなたは | 読むもの |
|---|---|
| はじめてモーターを回す | [はじめて回す](docs/guide/host/first-run.md) |
| はじめてサーボを動かす | [はじめてサーボを動かす](docs/guide/host/first-servo.md) |
| 配線の仕方を知りたい | [つなぎ方](docs/guide/wiring.md) |
| 動かなくて困っている | [動かないとき](docs/guide/host/troubleshooting.md) |
| 機体 1 台のプログラムを組む | [機体を組む](docs/guide/host/robot.md) |
| 主制御のプログラムを書いている | [案内](docs/guide/README.md) の「慣れてきたら」 |
| 手元の基板のことを知りたい | [今ある基板](docs/guide/boards.md) |
| 基板のファームを作る | [案内](docs/guide/README.md) の「基板のファームを作る」 |
| PC で線を見る、PC から指令する | [線を見る、PC から使う](docs/guide/pc.md) |
| 線の上のしくみ、決まりを知りたい | [仕様書](docs/spec/)。入門は [overview.md](docs/spec/overview.md) |
| このリポジトリを直す | [docs/internal/](docs/internal/) |

## 入れ方

PlatformIO のプロジェクトの `platformio.ini` に書きます。

ESP32:

```ini
[env:esp32]
platform = espressif32
board = esp32dev            ; ESP32-S3 は esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
lib_deps = https://github.com/autome-workspace/MAS_CAN_Library.git#v1.0.0
build_flags = -D CAN_TX=5 -D CAN_RX=4
```

Raspberry Pi Pico（RP2040）。CAN のためのライブラリ（ACAN2040）も足します。

```ini
[env:pico]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git#9c167c6b8aac4f4cfa6d55a0c4e5b848795150c0
board = pico
board_build.core = earlephilhower
framework = arduino
monitor_speed = 115200
lib_deps =
    https://github.com/autome-workspace/MAS_CAN_Library.git#v1.0.0
    https://github.com/obdevel/ACAN2040.git#1.0.6
build_flags = -D CAN_TX=0 -D CAN_RX=1
```

`build_flags` の行は、トランシーバにつないだ TX と RX のピンの番号です。プログラムの `CAN_TX`、`CAN_RX` に、この番号が入ります。例（`examples/`）は、どれもこの書き方です。

## 最小のプログラム

```cpp
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Md motor = can.md(0);

void setup() {
    can.begin(CAN_TX, CAN_RX);  // platformio.ini の build_flags で決めたピン
    can.waitReady(1000);
}

void loop() {
    can.update();  // 毎回呼ぶ
    motor.setDuty(0.3f);
}
```

覚えておくことは 3 つです。

- `can.update()` を、`loop()` で毎回呼びます。送ることも受けることも、この中で行います。
- `setDuty` などは、値を置くだけです。送るのは `update()` です。
- 単位は、速度が rad/s、電流が A、角度が rad です。duty は −1 から +1 です。

`can.begin(5, 4)` のように、ピンの番号を数字で直接書いても動きます。そのときは、`platformio.ini` の `build_flags` は要りません。ただ、数字で書くと、主制御を Pico に替えたときにプログラムも直すことになります（Pico の例のピンは 0 と 1）。`CAN_TX`、`CAN_RX` で書いておけば、直すのは `platformio.ini` だけです。

**配線から順に進める手順は、[はじめて回す](docs/guide/host/first-run.md)（モーター）と [はじめてサーボを動かす](docs/guide/host/first-servo.md) にあります。** はじめての人は、そちらから進めてください。

## 入っているもの

| 場所 | 中身 |
|---|---|
| [`lib/mas/`](lib/mas/) | ライブラリ。主制御の側（ESP32、RP2040）と、基板の側 |
| [`examples/`](examples/) | 例。最初は、回すだけの `md_minimal`。回らないときは、理由を表示する `md_check`。ほかに、サーボの `servo_minimal`、シリアルから試す `md_serial_test`、基板の側の雛形 `node_md_rp2040` |
| [`firmware/`](firmware/) | 基板のファーム。基板ごとに 1 フォルダで、使い方はそれぞれの README |
| [`docs/`](docs/) | 文書。地図は [docs/README.md](docs/README.md) |

前の版（v0、`MotorCAN`）とは互換がありません。v0 はタグ [`v0`](../../tree/v0) にあります。

## 直す人へ

- テスト: `make -C lib/mas/test`
- 決定の台帳、版とブランチの方針: [docs/internal/](docs/internal/)
