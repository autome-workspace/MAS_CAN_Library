# MAS CAN Library / Firmware

MAS のロボット向け CAN 通信の仕様、主制御のライブラリ、基板のファームウェア。

> [!WARNING]
> このブランチ（`next`）は **プロトコル v1 の作業中**（`1.0.0-alpha`）。旧版 v0 とは互換が無い。
> 今動いている v0 の一式は [`legacy/v0/`](legacy/v0/)（`main` ブランチにも同じものがある）。

| 場所 | 中身 |
|---|---|
| [`docs/decisions/`](docs/decisions/) | 仕様の正本（決定の台帳） |
| [`docs/versioning.md`](docs/versioning.md) | 版とブランチの方針 |
| [`lib/mas/`](lib/mas/) | ライブラリ（主制御側は書き直し中。基板側のコアは `core/`、`node`） |
| [`firmware/CAN_MD/`](firmware/CAN_MD/) | MD（STM32F042）の v1 のファーム |
| [`examples/`](examples/) | 使い方の例。API の見本は `api_tour` |
| [`legacy/v0/`](legacy/v0/) | 旧版 v0 の一式 |
