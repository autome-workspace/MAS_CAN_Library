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

## 開発

- テスト: `make -C lib/mas/test`
- 版とブランチ: [`docs/versioning.md`](docs/versioning.md)
- 未決: [`docs/decisions/open.md`](docs/decisions/open.md)
