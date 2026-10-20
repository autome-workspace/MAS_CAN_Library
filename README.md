# MAS CAN Library / Firmware

MAS のロボット向け CAN 通信の仕様、主制御のライブラリ、基板のファームウェア。

> [!WARNING]
> プロトコル v1（`1.0.0`）。旧版 v0 とは互換が無い。v0 の一式は [`legacy/v0/`](legacy/v0/) にある。

| 場所 | 中身 |
|---|---|
| [`docs/decisions/`](docs/decisions/) | 仕様の正本（決定の台帳） |
| [`docs/versioning.md`](docs/versioning.md) | 版とブランチの方針 |
| [`lib/mas/`](lib/mas/) | ライブラリ（主制御側の `Host`、基板側のコアは `core/`、`node`） |
| [`firmware/CAN_MD/`](firmware/CAN_MD/) | MD（STM32F042）の v1 のファーム |
| [`firmware/CAN-Servo-v1/`](firmware/CAN-Servo-v1/) | CAN-Servo（RP2040）の v1 のファーム |
| [`examples/`](examples/) | 使い方の例。API の見本は `api_tour` |
| [`legacy/v0/`](legacy/v0/) | 旧版 v0 の一式 |
