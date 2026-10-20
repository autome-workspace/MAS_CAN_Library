# CAN-Servo

サーボ 8 ch ＋ MD 4 ch（RP2040-Zero）。

| | |
|---|---|
| サーボ | J2〜J9 |
| MD（duty だけ） | J1、J10、J11、J12 |
| ch の先頭 | `platformio.ini` の `SERVO_FIRST_CH`、`MD_FIRST_CH` |

## ビルドと書き込み

```sh
pio run -t upload
```
