# MAS CAN Library / Firmware

MASのロボット向けCAN通信ライブラリ、デバイスファームウェア、通信仕様を管理するリポジトリです。ESP32から指令を送る`MotorCAN`ライブラリと、RP2040／STM32を使用した受信側ファームウェアを収録しています。

> [!WARNING]
> モーターやサーボは、主制御がすでに指令を送信している状態で基板を起動すると動き始める可能性があります。配線、書き込み、抜き差しを行う前に主制御を停止するか、E-stopを有効にしてください。

## 収録内容

| ディレクトリ | 対象 | 主な機能 | 開発環境 |
|---|---|---|---|
| [`lib/MotorCAN`](lib/MotorCAN/) | ESP32 | CAN指令の20 ms周期送信、E-stop、CAN送受信 | Arduino / PlatformIO |
| [`firmware/CAN-Servo`](firmware/CAN-Servo/) | RP2040-Zero | サーボ8ch、HブリッジDCモーター4ch | PlatformIO / Arduino-Pico |
| [`firmware/CAN_MD`](firmware/CAN_MD/) | STM32F042F6 | DCモーター1chの電流・duty制御、短絡ブレーキ、電流FB | CMakeまたはSTM32CubeIDE |
| [`test_code`](test_code/) | ESP32 | `MotorCAN`の最小使用例 | Arduino / PlatformIO |

## 共通CAN仕様

- CAN 2.0標準フレーム（11 bit ID）
- ビットレート: 1 Mbps
- 複数byte値: big-endian
- ID構成: `class(3 bit) | group(5 bit) | node(3 bit)`
- 駆動指令が100 ms途絶した場合は出力を停止
- E-stop: ID `0x000`。受信時はDLCにかかわらず直ちに出力を停止してラッチ
- E-stop解除: ID `0x001`、DLC 3、データ `63 6C 72`（ASCII `clr`）

CAN IDは次の式で生成します。

```text
can_id = (class << 8) | (group << 3) | node
```

駆動指令は`class = 2`、`node = 0`です。各ファームウェアの詳細なデータ形式と例外処理は、以下の仕様書を参照してください。

- CAN-Servo: [`firmware/CAN-Servo/main.md`](firmware/CAN-Servo/main.md)
- CAN_MD: [`firmware/CAN_MD/protocol.md`](firmware/CAN_MD/protocol.md)

> [!CAUTION]
> 現在、group 12～19の用途は両ファームウェアで一致していません。送信ライブラリとCAN-Servoではgroup 12～15をモーターduty、group 16～19を短絡ブレーキとして扱いますが、CAN_MDでは両者が逆です。現状の`setDuty()`／`setShortBrake()`をCAN_MDへそのまま使用すると誤動作するため、同じCANバスへ接続する前に割り当てを統一してください。

## ESP32送信ライブラリ

`MotorCAN`はESP32のTWAIドライバーを使用するArduino向けライブラリです。推奨名は`MasCan`で、既存コードとの互換性のため`MotorCAN`も使用できます。TXはGPIO 5、RXはGPIO 4が既定値です。

受信基板の100 msタイムアウトを防ぐため、設定された指令を内部FreeRTOSタスクから20 ms周期で自動再送します。アプリケーションの`loop()`で繰り返し送信する必要はありません。

### インストール

[`lib/MotorCAN`](lib/MotorCAN/)ディレクトリを、PlatformIOプロジェクトの`lib/MotorCAN`へコピーしてください。

```text
your-project/
├── lib/
│   └── MotorCAN/
│       ├── MasCan.h
│       ├── MotorCAN.h
│       ├── MotorCAN.cpp
│       └── library.json
└── src/
    └── main.cpp
```

Arduino IDEでは、同ディレクトリをスケッチブックの`libraries/MotorCAN`へコピーしてIDEを再起動します。

### 最小例

```cpp
#include <Arduino.h>
#include <MasCan.h>

MasCan can;  // TX: GPIO 5、RX: GPIO 4

void setup() {
    Serial.begin(115200);
    if (!can.begin()) {
        Serial.printf("CAN initialization failed: %d\n", can.lastError());
        return;
    }

    can.setCurrent(1, 100);       // モーター1へ100 mA
    can.setServoAngle(3, 90.0f);  // サーボCH3を90度へ
}

void loop() {}
```

別のGPIOを使用する場合は、コンストラクターへTX、RXの順で指定します。

```cpp
MasCan can(GPIO_NUM_21, GPIO_NUM_22);
```

### 主なAPI

| API | 内容 |
|---|---|
| `begin()` / `end()` | TWAIドライバーと自動送信タスクを開始／停止 |
| `setCurrent()` / `setSpeed()` / `setPosition()` | モーター1～16の電流／速度／位置指令を設定 |
| `setDuty()` / `setShortBrake()` | モーター1～16のduty／短絡ブレーキ指令を設定 |
| `setServo()` / `setServoPulseUs()` / `setServoAngle()` | サーボCH 1～32の出力を設定 |
| `disableServo()` | 値`255`を周期送信してサーボパルスを停止 |
| `setAir()` | エアー出力CH 1～32を設定 |
| `releaseMotor()` / `releaseServo()` / `releaseAir()` | 指定出力の周期送信を解除 |
| `releaseAll()` | すべての周期送信を解除 |
| `emergencyStop()` / `clearEmergencyStop()` | E-stopを送信／解除 |
| `sendRaw()` / `receive()` | 任意の標準CANフレームを送信／受信 |

`emergencyStop()`を呼ぶとローカルでもE-stopがラッチされ、`clearEmergencyStop()`の送信に成功するまで新しい出力設定は拒否されます。詳しい使用例は[`lib/MotorCAN/README.md`](lib/MotorCAN/README.md)を参照してください。

## CAN-Servo

RP2040-ZeroとATA6561を使用し、サーボ8chとDCモーター4chを制御します。

### 既定設定

| 項目 | 値 |
|---|---|
| CAN TX / RX | GP0 / GP1 |
| サーボ出力（GN1～GN8） | GP29, GP28, GP27, GP26, GP15, GP14, GP13, GP12 |
| モーター出力ペア（GN1～GN4） | GP10/11, GP8/9, GP6/7, GP4/5 |
| サーボgroup / CAN ID | 20 / `0x2A0` |
| モーターduty group / CAN ID | 12 / `0x260` |

サーボ指令は8 byte固定です。各byteの`0～200`を500～2500 µsのパルス幅へ変換し、`255`で該当出力を停止します。モーター指令は4個の符号付き16 bit値をbig-endianで格納し、符号を回転方向、絶対値をPWM dutyとして扱います。

オンボードLEDは、待機中が緑、有効な指令の受信中が青、E-stopラッチ中が赤です。

### ビルドと書き込み

PlatformIO CLIをインストールした環境で実行します。初回ビルド時にはArduino-PicoとACAN2040が取得されます。

```sh
cd firmware/CAN-Servo
pio run
pio run --target upload
```

groupを変更する場合は、[`platformio.ini`](firmware/CAN-Servo/platformio.ini)の`SERVO_CAN_GROUP`と`MOTOR_CAN_GROUP`を編集してください。

## CAN_MD

STM32F042F6とDRV8701Pを使用する1ch DCモータードライバーです。

### 主な仕様

- 3 bit DIPスイッチで基板番号1～8を設定（`000`は8）
- 電流指令: `-2400～2400 mA`（範囲外は飽和）
- duty指令: 符号付き16 bit全域を約`-100～+100%`へ変換
- 短絡ブレーキ: `0`で解除、`1`で適用
- 電流フィードバック: 200 Hz、DLC 2、mA単位
- bus-offからはbxCANのAutoBusOffで自動復帰

基板番号`n`に対するgroupとnodeは次のように決まります。

```text
group(current) = 0  + floor((n - 1) / 4)
group(brake)   = 12 + floor((n - 1) / 4)
group(duty)    = 16 + floor((n - 1) / 4)
node           = 1  + ((n - 1) % 4)   # フィードバック用
```

### CMakeでビルド

Arm GNU ToolchainとNinjaが必要です。

```sh
cd firmware/CAN_MD
cmake --preset Debug
cmake --build --preset Debug
```

Releaseビルドでは、両コマンドの`Debug`を`Release`へ置き換えます。STM32CubeIDE用プロジェクトとIAR EWARM用プロジェクトも同ディレクトリに含まれています。

### 既知のハードウェア制約

- 現状はHSE発振不良のため16 MHz HSIを使用しており、CAN仕様で推奨する水晶クロックとクロック偏差の条件を満たしていません。水晶回路の修理後にHSEへ戻す必要があります。
- PB8/BOOT0のLowが保証されず、リセット時に内蔵ブートROMへ入る場合があります。PB8/BOOT0へ直接プルダウン抵抗を追加してください。
- 基板全体の安全な連続電流定格は未確定です。回路上の理論値ではなく、ファームウェアの`±2.4 A`制限を上限として扱ってください。

## 配線上の注意

MCUのGPIOをCAN_H / CAN_Lへ直接接続することはできません。対応するCANトランシーバーを介して接続し、バスの両端を120 Ωで終端してください。正常な通信には、同じビットレートで動作しACKを返す別ノードも必要です。
