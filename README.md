# MAS CAN Library / Firmware

MAS のロボット向け CAN 通信（プロトコル v1）の仕様、主制御のライブラリ、基板のファームウェア。

v0（`MotorCAN`）とは互換が無い。v0 はタグ [`v0`](../../tree/v0)。

## 収録内容

| 場所 | 中身 |
|---|---|
| [`lib/mas/`](lib/mas/) | ライブラリ（主制御 ESP32 / RP2040、基板側のコア） |
| [`firmware/`](firmware/) | 基板のファーム。基板ごとに 1 フォルダで、使い方と制約はその README |
| [`examples/`](examples/) | 例。まずは `md_minimal`、`servo_minimal` |
| [`docs/decisions/`](docs/decisions/) | 仕様の正本 |

## 使い方

`platformio.ini`（ESP32）:

```ini
[env:esp32]
platform = espressif32      ; PlatformIO 公式（arduino-esp32 2.x 系）
board = esp32dev            ; ESP32-S3 は esp32-s3-devkitc-1
framework = arduino
lib_deps = https://github.com/autome-workspace/MAS_CAN_Library.git#v1.0.0
```

RP2040 は earlephilhower のコアを使い、ACAN2040 も足す。

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

```cpp
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Md motor = can.md(0);

void setup() {
    can.begin(5, 4);  // TX, RX
    can.waitReady(1000);
}

void loop() {
    can.update();  // 毎回呼ぶ
    motor.setDuty(0.3f);
}
```

- `can.update()` を `loop()` で毎回呼ぶ（100 Hz 以上）。
- `set…` は値を置くだけ。送るのは `update()`。
- 単位は SI（rad/s、A、rad）。duty は −1〜+1。

`can.begin(tx, rx)` は近道。CAN の口を自分で作って渡すこともできる。

```cpp
mcan::Esp32Twai bus;  // ESP32（TWAI）

void setup() {
    bus.begin(GPIO_NUM_5, GPIO_NUM_4);  // TX, RX
    can.begin(bus);
}
```

```cpp
mcan::Rp2040Can2040 bus;  // RP2040（can2040。PIO 0 と 1 で 2 本まで）

void setup() {
    bus.begin(5, 4, 1);  // TX, RX, PIO
    can.begin(bus);
}
```

| API | 内容 |
|---|---|
| `can.begin(tx, rx)` | 開始。失敗の理由は `can.error()` |
| `can.waitReady(ms)` | MD の準備を待つ |
| `can.estop()` / `can.clearEstop()` | E-stop / 解除 |
| `can.md(ch)` | `setDuty`、`setCurrent`、`setSpeed`、`stop`、`feedback`、`online` |
| `can.servo(ch)` | `setAngle`、`setPulseUs`、`release`、`configCalibration` |
| `can.onFault(fn)` | 基板の異常 |

全体は [`docs/decisions/api.md`](docs/decisions/api.md)。

## 開発

- テスト: `make -C lib/mas/test`
- 版とブランチ: [`docs/versioning.md`](docs/versioning.md)
- 未決: [`docs/decisions/open.md`](docs/decisions/open.md)
