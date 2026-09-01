# MAS CAN Library

ESP32 の TWAI（CAN）コントローラーから、CAN 接続されたモーターへ電流指令とショートブレーキ指令を送信する Arduino 向けライブラリです。

## 主な機能

- モーター 1～16 への電流指令
- モーター 1～16 へのショートブレーキ指令
- CAN 2.0 標準フレーム（11 bit ID）を使用
- ESP32 標準の TWAI ドライバーを利用

## 動作環境

- ESP32
- Arduino framework for ESP32
- `driver/twai.h` を利用できる ESP32 ボードパッケージ
- 1 Mbps に対応した CAN トランシーバー

> [!IMPORTANT]
> ESP32 の GPIO を CAN_H / CAN_L に直接接続することはできません。ESP32 と CAN バスの間に、3.3 V ロジックに対応した CAN トランシーバーを接続してください。また、バスの両端には適切な終端抵抗（一般に 120 Ω）が必要です。

## ディレクトリ構成

```text
MAS_CAN_Library/
├── lib/
│   └── MotorCAN/
│       ├── MotorCAN.cpp
│       └── MotorCAN.h
└── test_code/
    └── main.cpp
```

## インストール

### PlatformIO

このリポジトリを PlatformIO プロジェクトとして使用する場合は、`lib/MotorCAN` をそのままプロジェクトの `lib` ディレクトリ内に配置します。

```text
your_project/
├── lib/
│   └── MotorCAN/
│       ├── MotorCAN.cpp
│       └── MotorCAN.h
└── src/
    └── main.cpp
```

### Arduino IDE

`lib/MotorCAN` フォルダーを Arduino のスケッチブックにある `libraries` フォルダーへコピーし、Arduino IDE を再起動します。


## 基本的な使い方

```cpp
#include <Arduino.h>
#include "MotorCAN.h"

MotorCAN motorCAN(GPIO_NUM_5, GPIO_NUM_4);  // TX, RX
bool motorCANReady = false;

void setup() {
    Serial.begin(115200);

    motorCANReady = motorCAN.begin();
    if (!motorCANReady) {
        Serial.printf("TWAI initialization failed: %d\n",
                      static_cast<int>(motorCAN.lastError()));
    }
}

void loop() {
    if (!motorCANReady) {
        delay(1000);
        return;
    }

    // モーター 1 に 20 mA の電流指令を送信
    if (!motorCAN.setCurrent(1, 20)) {
        Serial.printf("CAN transmission failed: %d\n",
                      static_cast<int>(motorCAN.lastError()));
    }

    delay(20);
}
```

`begin()` は TWAI ドライバーを 1 Mbps、ノーマルモードで初期化して開始します。戻り値が `false` の場合は、それ以降の送信を行わず `lastError()` を確認してください。

### コンストラクター

```cpp
MotorCAN(gpio_num_t txPin, gpio_num_t rxPin);
```

使用する TWAI の TX GPIO と RX GPIO を指定します。

### `begin()`

```cpp
bool begin();
```

TWAI ドライバーをインストールし、通信を開始します。通信速度は **1 Mbps 固定**です。成功時は `true`、失敗時は `false` を返します。

### `setCurrent()`

```cpp
bool setCurrent(uint8_t motorNumber, int16_t currentMilliAmps);
```

指定したモーターへ電流指令を送信します。

| 引数 | 内容 |
|---|---|
| `motorNumber` | モーター番号（1～16） |
| `currentMilliAmps` | 電流指令値 [mA]（符号付き 16 bit） |

送信成功時は `true`、失敗時は `false` を返します。電流値の許容範囲や符号と回転方向の関係は、接続するモーター側の仕様に従ってください。

### `setShortBrake()`

```cpp
bool setShortBrake(uint8_t motorNumber, bool enabled);
```

指定したモーターのショートブレーキ状態を送信します。

- `enabled = true`: ショートブレーキを適用
- `enabled = false`: ブレーキを解除してコースト状態に移行

この指令は受信側で保持されることを前提とした指令です。

### `lastError()`

```cpp
esp_err_t lastError() const;
```

直前に実行した初期化または送信処理の ESP-IDF エラーコードを返します。たとえば、モーター番号が 1～16 の範囲外の場合は `ESP_ERR_INVALID_ARG` になります。

## 使用例

```cpp
motorCAN.setCurrent(1, 20);        // モーター 1 に 20 mA
motorCAN.setCurrent(2, -100);      // モーター 2 に -100 mA
motorCAN.setShortBrake(1, true);   // モーター 1 のブレーキを適用
motorCAN.setShortBrake(1, false);  // モーター 1 のブレーキを解除
```

## CAN フレーム仕様

モーターは 4 台ずつのグループに分かれ、各モーターの値は 2 byte のビッグエンディアン形式で格納されます。

| 指令 | モーター番号 | CAN ID |
|---|---:|---:|
| 電流 | 1～4 | `0x200` |
| 電流 | 5～8 | `0x208` |
| 電流 | 9～12 | `0x210` |
| 電流 | 13～16 | `0x218` |
| ショートブレーキ | 1～4 | `0x260` |
| ショートブレーキ | 5～8 | `0x268` |
| ショートブレーキ | 9～12 | `0x270` |
| ショートブレーキ | 13～16 | `0x278` |

ショートブレーキの値は、適用時が `1`、解除時が `0` です。

### 同一グループ内の送信に関する注意

`setCurrent()` と `setShortBrake()` は、指定したモーターのスロットまでを含む DLC でフレームを送信し、それより前のスロットを `0` で埋めます。

たとえばモーター 2 へ指令を送ると、同じグループにあるモーター 1 のスロットには `0` が入ります。このため、同一グループ内の複数モーターを制御する場合、後ろの番号への送信によって前の番号の指令が意図せず変化する可能性があります。アプリケーション側で送信順序や送信タイミングを管理してください。

## 注意事項

- モーター番号に指定できるのは 1～16 です。
- 本ライブラリの通信速度は 1 Mbps 固定です。接続するすべての機器で通信速度を一致させてください。
- `begin()` を複数回呼び出すことは想定していません。
- 現在の API には TWAI ドライバーを停止・アンインストールする終了処理はありません。
- CAN バス上で ACK を返す別ノードが存在しない場合、通信は正常に成立しません。
