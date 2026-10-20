# MAS CAN Library / Firmware

MAS のロボット向け CAN 通信（プロトコル v1）の仕様、主制御のライブラリ、基板のファームウェア。

> [!WARNING]
> 基板は電源を入れると動く。配線や書き込みの前に駆動電源を切るか E-stop を掛ける。

v0（`MotorCAN`）とは互換が無い。v0 はタグ [`v0`](../../tree/v0)。

## 収録内容

| 場所 | 中身 |
|---|---|
| [`lib/mas/`](lib/mas/) | ライブラリ（主制御 ESP32 / RP2040、基板側のコア） |
| [`firmware/CAN_MD/`](firmware/CAN_MD/) | MD 1 ch（STM32F042） |
| [`firmware/CAN-Servo-v1/`](firmware/CAN-Servo-v1/) | サーボ 8 ch ＋ MD 4 ch（RP2040-Zero） |
| [`examples/`](examples/) | 例。まずは `md_minimal`、`servo_minimal` |
| [`docs/decisions/`](docs/decisions/) | 仕様の正本 |

## 使い方

`platformio.ini`:

```ini
lib_deps = git@github.com:autome-workspace/MAS_CAN_Library.git#v1.0.0
```

RP2040 は ACAN2040 も足す。

```ini
lib_deps =
    git@github.com:autome-workspace/MAS_CAN_Library.git#v1.0.0
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

| API | 内容 |
|---|---|
| `can.begin(tx, rx)` | 開始。失敗の理由は `can.error()` |
| `can.waitReady(ms)` | MD の準備を待つ |
| `can.estop()` / `can.clearEstop()` | E-stop / 解除 |
| `can.md(ch)` | `setDuty`、`setCurrent`、`setSpeed`、`stop`、`feedback`、`online` |
| `can.servo(ch)` | `setAngle`、`setPulseUs`、`release`、`configCalibration` |
| `can.onFault(fn)` | 基板の異常 |

全体は [`docs/decisions/api.md`](docs/decisions/api.md)。

## 基板

### CAN_MD

| | |
|---|---|
| ch | DIP で 0〜7。LED が ch の回数だけ点滅 |
| 指令 | duty、電流（速度は不可） |
| 電流の上限 | 15 A（既定も 15 A） |

```sh
cd firmware/CAN_MD
cmake --preset Release && cmake --build --preset Release
```

新しい基板には 1 回 `STM32_Programmer_CLI -c port=SWD -ob BOOT_SEL=0` を書く。

### CAN-Servo

| | |
|---|---|
| サーボ | J2〜J9 |
| MD（duty だけ） | J1、J10、J11、J12 |
| ch の先頭 | `platformio.ini` の `SERVO_FIRST_CH`、`MD_FIRST_CH` |

```sh
cd firmware/CAN-Servo-v1
pio run -t upload
```

## 既知の制約

- CAN_MD は HSI で動いている（CAN にはクロックの精度が足りない）。
- CAN_MD の電流の上限の既定が 15 A。
- CAN_MD は小さな電流の指令で duty 0.345 より下がらない。

ほかは [`open.md`](docs/decisions/open.md)、[`TODO.md`](firmware/CAN_MD/TODO.md)。

## 開発

- テスト: `make -C lib/mas/test`
- 版とブランチ: [`docs/versioning.md`](docs/versioning.md)
