# 実装の読み方の案内

2026-10-07 時点。全体は約 3,300 行で、そのうち本体は約 2,000 行。読む順番と、それぞれの見どころをまとめる。

## 全体の地図

```
lib/mas/
  include/mas/can/core/   ← ① コア（C）: フレームの形。基板と主制御の共通部分
  src/can/core/
  include/mas/can/*.h     ← ② 主制御側（C++）: Bus, Md, Controller
  src/can/*.cpp
  src/can/node.c          ← ③ 基板側（C）: 指令を受けて、出すべき出力を決める
  include/mas/can/port.h  ← ④ ハードウェアとの境目
  src/can/esp32_twai.cpp
  test/                   ← ⑤ PC で動くテスト（使い方の実例）
firmware/CAN_MD/App/app.c ← ⑥ 基板のファーム（STM32 の HAL で ③ をつなぐ）
examples/md_serial_test/  ← ⑦ 主制御のスケッチ（② を使う側）
```

## 今できているもの

| 層 | できているもの | まだ無いもの |
|---|---|---|
| ① コア | ID の計算、ビッグエンディアンの詰め書き。E-stop、解除、SYNC、起動の知らせ、設定のフレーム。MD の指令と FB のフレーム | MD 以外の種類のフレーム |
| ② 主制御側 | `Bus`（E-stop、SYNC、設定の送受信と確認、再起動の後始末）、`Md`（32 ch 全部。電流・速度・duty の指令、FB、位置の積み上げ、異常）、`Controller`（受け取り） | MD とコントローラ以外の種類。保存する設定（A5）。使う ch の分だけ確保する方針（library-api-draft.md の A1 の補足） |
| ③ 基板側 | MD の種類（複数 ch の基板にも対応した作り。実機で動いているのは 1 ch の基板） | MD 以外の種類 |
| ④ ポート | ESP32 の TWAI（初代 ESP32 と ESP32-S3 でビルド、初代 ESP32 は実機で確認） | STM32 の部品。今は ⑥ に直接書いてある |

PC のテストは 3 本（コア、主制御側、基板側）とも通る。

## おすすめの読み方

### 1. 使う側から見る（約 30 分）

- [`examples/md_serial_test/src/main.cpp`](../../examples/md_serial_test/src/main.cpp)（100 行）: ライブラリを使う側のコード。`can.begin`、`can.md(0)`、`md.setDuty`、`can.update()` という流れをつかむ。
- [`lib/mas/include/mas/can/md.h`](../../lib/mas/include/mas/can/md.h)、[`bus.h`](../../lib/mas/include/mas/can/bus.h): 公開している API の一覧。宣言だけを眺める。

### 2. フレームの形を見る（約 30 分）

- [`include/mas/can/core/frame.h`](../../lib/mas/include/mas/can/core/frame.h)（101 行）: ID の計算（`mas_can_setting_id` など）と、ビッグエンディアンの詰め書き。文書の ID の表と一致しているかを確かめる。
- [`src/can/core/md.c`](../../lib/mas/src/can/core/md.c)（107 行）: MD の指令と FB の組み立てと読み取り。仕様の byte の並びと照らし合わせるのに向く。

### 3. 主制御側の中身（約 1 時間）

[`src/can/bus.cpp`](../../lib/mas/src/can/bus.cpp)（400 行）。中心は `Bus::update()`（52 行目）で、ここから呼ばれる順に追うと読みやすい。

- `receiveAll`: 受けたフレームの振り分け
- `handleMdFeedback`: FB の取り込み、位置の積み上げ
- `sendCommands`: 指令の送信、止める値への差し替え
- `serviceEstop`: E-stop を 10 Hz で送り続ける（A8）
- `serviceSettings`: 設定を 1 つずつ送り、応答を確かめる（A4）

### 4. 基板側の中身（約 1 時間）

[`src/can/node.c`](../../lib/mas/src/can/node.c)（510 行）。入口は 3 つ。

- `mas_can_node_on_frame`（401 行目）: 受けたフレームの処理
- `mas_can_node_tick`（451 行目）: 時間による処理（途絶の判定、FB の送信の時期）
- `mas_can_md_output`（486 行目）: 「今出すべき出力」を返す。止める理由（E-stop、途絶、起動直後）がここで効く

設定の要求への応答は `handle_setting`（337 行目）。

### 5. 実機のファーム（約 30 分）

[`firmware/CAN_MD/App/app.c`](../../firmware/CAN_MD/App/app.c)（362 行）。③ の node と、STM32 の HAL をつなぐ部分。

- `app_loop`（342 行目）: 受信 → node → 出力 → 測定 → 送信の流れ
- `ADC1_IRQHandler`（178 行目）、`current_loop`（152 行目）: 電流の制御（md-firmware-draft.md の §5.4.1）

行番号は 2026-10-07 時点のもの。ずれていたら関数名で探す。

## 読むときのコツ

- **テストを仕様書として読む.** 例えば [`test/test_bus.cpp`](../../lib/mas/test/test_bus.cpp) の `test_estop_and_sync` は、「E-stop を掛けると 0.3 秒で 3 回送り、その間の指令は 0 になる」ことを、そのままコードで確かめている。本体で迷ったら、対応するテストを見ると意図がわかる。
- **テストは手元で動かせる.** `make -C lib/mas/test` で 3 本とも動く。本体を少し書き換えて、テストが落ちるのを見るのも理解の助けになる。
- **文書と照らし合わせる.** 決定の番号（D10、A4 など）がコメントに入っているところは、[requirements.md](requirements.md) や [library-api-draft.md](library-api-draft.md) の該当する行と対応している。

## 見てほしい観点

- 名前と書き方が読みやすいか（コメントの量も含めて）
- 文書の決定と、コードの動きが食い違っていないか
- 「ここは自分では保守できない」と感じる箇所はどこか
