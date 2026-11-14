# やりたいこと別

> **未執筆。** 下の枠に沿って書く。書き方は [writing.md](../../internal/writing.md)。書き終えたら、この引用の段を消す。

| | |
|---|---|
| 読む人 | 主制御を書く部員 |
| 前提 | 「はじめて回す」を読んだ |
| 読むとできること | 速度で回す、電流を絞る、E-stop、基板を増やす、異常を拾う、複数の線を使う、などを、それぞれ短い手順でできる |
| 見せないもの | ID、フレーム |
| 材料 | internal/decisions/api.md、lib/mas/include/mas/can/ のヘッダ、examples/api_tour（密度が高いので、そのまま見せない） |

## 書くときの材料: pioarduino（2026-11-15、Claude が確かめた）

「触れる程度に書く」（作者）。事実だけ:

- ESP32 は、ほとんどの場合、PlatformIO 公式の環境（`platform = espressif32`。arduino-esp32 2.x）で困らない。
- 新しい版（arduino-esp32 3.x）を使うには、有志の pioarduino が要る。今のライブラリは、pioarduino 55.03.312（arduino-esp32 3.3.12、ESP-IDF 5.5.5）で、無変更でビルドできた（`examples/md_minimal`。実機では未確認）。
- **pioarduino のプラットフォームを使うと、PC の PlatformIO 本体が pioarduino 版に入れ替わる。** プラットフォームが、自分の本体（`pioarduino` という Python のパッケージ）を、共有の環境（`~/.platformio/penv`）に入れる作りになっている（プラットフォームの `builder/penv_setup.py`。パッケージの説明にも「pioarduino core is needed to run pioarduino Platform espressif32」とある）。ほかのプロジェクトにも影響する。戻すには、`pioarduino` を消して `platformio` を入れ直す。
- そのため、このリポジトリの例には pioarduino の環境を入れない（例のフォルダで `pio run` をすると全部の環境がビルドされ、知らないうちに本体が入れ替わるため）。
- 新しい版では、CAN の古い書き方のドライバ（`driver/twai.h`）は非推奨の扱い（ヘッダの名前が `twai_types_deprecated.h`）。今は動く。
