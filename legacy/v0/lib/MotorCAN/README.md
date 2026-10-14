# MotorCAN

ESP32 TWAI用のMAS CAN送信ライブラリです。CANはstandard 11-bit、1 Mbps、
TX GPIO5／RX GPIO4を既定値として使用します。

受信基板には100 msの指令タイムアウトがあるため、設定済み出力を内部タスク
から20 ms周期で自動再送します。`loop()`から定期送信する必要はありません。

## 最小サーボ例

```cpp
#include <Arduino.h>
#include <MasCan.h>

MasCan can;

void setup() {
    can.begin();
    can.setServo(3, 100);  // CH3 (GP27), 1500 us
}

void loop() {}
```

一度設定した出力は`release...()`、`disableServo()`、E-stop、またはESP32の
停止まで自動送信され続けます。DCモーターを扱うアプリケーションでは、物理的な
非常停止入力も必ず用意してください。

`setServo(channel, value)`のvalueは0..200で、パルス幅は
`500 + value * 10` usです。

```cpp
can.setServoAngle(3, 90.0f);
can.setServoPulseUs(3, 1500);
can.disableServo(3);
```

## DCモーター

```cpp
can.setCurrent(1, 100);
can.setSpeed(1, 200);
can.setPosition(1, 500);
can.setDuty(4, 8192);       // J12
can.setShortBrake(1, true);
```

## エア・非常停止

```cpp
can.setAir(1, true);
can.setAir(1, false);
can.emergencyStop();
can.clearEmergencyStop();
```

E-stop後は`clearEmergencyStop()`が成功するまで新しい出力設定を拒否します。
通信更新を止める場合は`releaseServo()`、`releaseMotor()`、`releaseAir()`、
または`releaseAll()`を使用します。対象基板は100 ms後に停止します。
