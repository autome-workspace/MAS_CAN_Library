# MAS CAN

MAS のロボットで、主制御のマイコンから、モータードライバーやサーボの基板を動かすための決まりと、そのライブラリです。主制御と基板を 2 本の線（CAN）でつなぎ、主制御のプログラムは、基板の番号を指定して動かします。

```cpp
mcan::Md motor = can.md(0);   // モータードライバーの 0 番

void loop() {
    can.update();
    motor.setDuty(0.3f);      // 30% の強さで回す
}
```

## どこを読むか

| あなたは | 読むもの |
|---|---|
| はじめて使う | [はじめて回す](docs/guide/host/first-run.md) |
| 動かなくて困っている | [動かないとき](docs/guide/host/troubleshooting.md) |
| 主制御のプログラムを書いている | [案内](docs/guide/README.md) の「主制御のプログラムを書く」 |
| 手元の基板のことを知りたい | [今ある基板](docs/guide/boards.md) |
| 基板のファームを作る | [案内](docs/guide/README.md) の「基板のファームを作る」 |
| 決まりを正確に知りたい | [仕様書](docs/spec/) |
| このリポジトリを直す | [docs/internal/](docs/internal/) |

## 入れ方

PlatformIO のプロジェクトの `platformio.ini` に書きます。

ESP32:

```ini
[env:esp32]
platform = espressif32
board = esp32dev            ; ESP32-S3 は esp32-s3-devkitc-1
framework = arduino
lib_deps = https://github.com/autome-workspace/MAS_CAN_Library.git#v1.0.0
```

Raspberry Pi Pico（RP2040）。CAN のためのライブラリ（ACAN2040）も足します。

```ini
[env:pico]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git#9c167c6b8aac4f4cfa6d55a0c4e5b848795150c0
board = pico
board_build.core = earlephilhower
framework = arduino
lib_deps =
    https://github.com/autome-workspace/MAS_CAN_Library.git#v1.0.0
    https://github.com/obdevel/ACAN2040.git#1.0.6
```

## 最小のプログラム

```cpp
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Md motor = can.md(0);

void setup() {
    can.begin(5, 4);  // CAN の TX、RX のピン
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

配線から順に進める手順は、[はじめて回す](docs/guide/host/first-run.md) にあります。

`can.begin(5, 4)` は近道です。CAN の口を自分で作って、渡すこともできます。線を 2 本使うときなどに使います。

```cpp
mcan::Esp32Twai bus;  // ESP32

void setup() {
    bus.begin(GPIO_NUM_5, GPIO_NUM_4);  // TX、RX
    can.begin(bus);
}
```

```cpp
mcan::Rp2040Can2040 bus;  // RP2040（PIO 0 と 1 で、2 本まで）

void setup() {
    bus.begin(5, 4, 1);  // TX、RX、PIO
    can.begin(bus);
}
```

## 入っているもの

| 場所 | 中身 |
|---|---|
| [`lib/mas/`](lib/mas/) | ライブラリ。主制御の側（ESP32、RP2040）と、基板の側 |
| [`examples/`](examples/) | 例。最初は `md_minimal` と `servo_minimal`。線の上の基板を一覧にする `md_scan`、シリアルから試す `md_serial_test`、基板の側の雛形 `node_md_rp2040` |
| [`firmware/`](firmware/) | 基板のファーム。基板ごとに 1 フォルダで、使い方はそれぞれの README |
| [`docs/`](docs/) | 文書。地図は [docs/README.md](docs/README.md) |

前の版（v0、`MotorCAN`）とは互換がありません。v0 はタグ [`v0`](../../tree/v0) にあります。

## 直す人へ

- テスト: `make -C lib/mas/test`
- 決定の台帳、版とブランチの方針: [docs/internal/](docs/internal/)
