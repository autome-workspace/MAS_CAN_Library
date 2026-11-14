# 基板のファームを作る

新しい基板（MD など）のファームを書く人と、既存のファームを直す人のための手引き。基板が守ることと、`lib/mas` の基板側のコア（node）の使い方をまとめる。フレームの形や設定の項目の細部は次のファイルに任せる。

| ファイル | 中身 |
|---|---|
| [overview.md](overview.md) | 全体像と用語 |
| [bus.md](bus.md) | 物理層、E-stop、SYNC、起動の知らせ、起動の猶予、周期指令の共通の規則、再起動 |
| [ids.md](ids.md) | ID の一覧 |
| [settings.md](settings.md) | 設定の仕組み、全項目、異常の詳細の bit |
| [md.md](md.md)、[servo.md](servo.md) | 種類ごとの指令、FB、動作 |

根拠は台帳の番号（C-、M-、S-）で書く。node の API は台帳に無く、コードが正本なので「（node.h）」「（node.c）」と書く。「今の実装では」と書いた文と「既存のファーム」の節は、`lib/mas` の node と各ファームの今の動きで、台帳の決まりではない。台帳で決まっていないことは「台帳では未定」と書く。

> **基板の情報は設定で読む。** 基板の種類、版、ファームの版、固有 ID、受け持つ ch は、class 4 の設定の共通の項目（0〜6）で読む（C-60）。MD なら要求 `0x400`、応答 `0x408`（C-46）。`0x300` 台（class 3）は周期データで、MD では ch ごとの FB（`0x320` ＋ ch、M-14）しか無い。基板の情報は載らない。読み方の例は「[試し方](#試し方)」。

## 目次

- [立場と分担](#立場と分担)
- [守ることの確認表](#守ることの確認表)
- [基板の資料に書くこと](#基板の資料に書くこと)
- [受信フィルタ](#受信フィルタ)
- [node を使う](#node-を使う)
- [ビルドへの組み込み](#ビルドへの組み込み)
- [node が持たない種類（サーボなど）](#node-が持たない種類サーボなど)
- [既存のファーム](#既存のファーム)
- [試し方](#試し方)

## 立場と分担

| 名前 | 何か |
|---|---|
| 主制御 | 指令を出す側（ESP32 など）。主制御のライブラリ（`mas::can::Host`）を使う |
| 基板 | 指令を受けて動く側（MD など） |
| node | 基板側のコア。純粋な C で、ハードウェアに触らない（`lib/mas/src/can/node.c`） |
| ポート | 基板のファームのうち、ハードウェアに触る部分（CAN、時計、k、フラッシュ、モーター） |

node はフレームと時刻を受け取り、送るフレームと「モーターに何を出すべきか」を返す（node.h）。基板のファームは node とポートでできる。

## 守ることの確認表

「node」の列は、node を使えば node が受け持つもの（○）、ポートが受け持つもの（ポート）、両方が要るもの（両方）。

### 物理層

| 守ること | 根拠 | node |
|---|---|---|
| 通常の CAN（FD ではない、8 byte まで）、11 bit の標準 ID。拡張 ID のフレームは node に渡さない（`mas_can_frame_t` には標準 ID と拡張 ID を区別する欄が無い。リモートフレームの区別は `rtr` の欄にある） | C-8 | ポート |
| 1 Mbps、サンプル点 80%。can2040 だけは 81.25% 固定を認める。SJW は取れる最大（フェーズセグメント 2 と同じ） | C-9 | ポート |
| 発振子は水晶を推奨。セラロックなら ±0.5% 級 | C-10 | ポート |
| データフレームだけを使う。リモートフレームは何もしない。ただし `0x000` はリモートフレームでも E-stop。フィルタはリモートフレームの `0x000` を通す。ポートは受けたフレームの `rtr` を必ず埋める（リモートフレームかどうかは node がこの欄で判定する。埋めないと、フィルタを通ったリモートフレームを指令や設定の要求として受けてしまう） | C-11 | 両方（`rtr` を埋めるのはポート） |
| 2 byte 以上の値は big-endian | C-12 | ○ |

### 基板の位置 k と ch

| 守ること | 根拠 | node |
|---|---|---|
| k の決め方（DIP、LED とスイッチなど）は基板ごとに決め、基板の資料に書く。起動時に k が決まっていればよい | C-75 | ポート |
| k は起動時に 1 回だけ読む | C-18 | ポート |
| 主な種類の先頭の ch ＝ k × 基板の大きさ（受け持つ ch の数）。他の種類の ch の範囲は、ファームが持つ規則で k から計算する | C-81 | ポート |
| 先頭の ch ＋ 基板の大きさがその種類の ch の数（MD は 32）を超えるなら、CAN に何も送らず何も受けず、出力を止めたままにする。知らせ方は基板の資料に書く | C-18、M-1 | ポート |
| k の bit 数はプロトコルで決めない。推奨は、その種類の ch の数を基板の大きさで割った区画をすべて指せる数 | C-82 | ポート |
| ch と k は 0 から数える | C-13 | ポート |

### 基板の情報（設定の共通の項目）

| 守ること | 根拠 | node |
|---|---|---|
| 固有 ID（項目 0）: 96 bit の固有番号（w0、w1、w2）を持つ MCU は `((w2 XOR w1) << 32) \| w0`。RP2040 はフラッシュの 64 bit の固有 ID をそのまま、先頭を最上位にする | C-62 | ポート（値を作る） |
| C-62 に無いマイコンは工場で書かれた固有の番号を使い、64 bit より短ければ上位を 0 で埋める（ESP32 なら MAC の 48 bit）。どれを使うかは基板の資料に書く | C-86 | ポート |
| 項目 0 の応答は data 8 byte すべてが固有 ID | C-61 | ○ |
| 基板の種類（項目 1）: 上位 16 bit ＝ 基板の種類、下位 16 bit ＝ 基板の版（1 から）。番号は一覧に書き足して取る（下の「登録の手順」） | C-63、C-93 | ポート（値を渡す） |
| ファームの版（項目 2）: byte 3 ＝ 0、byte 4 ＝ 大、byte 5 ＝ 中、byte 6 ＝ 小。ファームの区別はこれで行う | C-70、C-73 | ポート（値を渡す） |
| プロトコルの版（項目 3）は node が自動で返す。ファームには書かない | C-93、C-71 | ○ |
| 受け持つ ch の範囲（項目 4）: 要求の種類について、byte 3–4 ＝ 先頭の ch、byte 5–6 ＝ ch の数 | C-65 | ○ |

**基板の種類の登録の手順**

1. 番号を一覧に書き足して取る（C-63）。一覧は台帳と `lib/mas/include/mas/can/core/board.h` の両方にあり、同じにする（C-93）。今の一覧は `0x0000` 予約、`0x0001` CAN_MD 基板（C-97）、`0x0002` CAN-Servo 基板、`0x0003` micro-MD 基板（C-63、C-92、C-96）。
2. `core/board.h` に `#define MAS_CAN_BOARD_xxx 0x000Nu` を足す。
3. ファームで `cfg.board_type = MAS_CAN_BOARD_TYPE(MAS_CAN_BOARD_xxx, 版)` とする。

登録するまでの雛形は `MAS_CAN_BOARD_RESERVED`（`0x0000`）のまま（core/board.h）。

### 起動

| 守ること | 根拠 | node |
|---|---|---|
| 電源投入では出力は止まっている。最初の有効な指令から動く | M-22、S-7 | 両方（止めた状態で始めるのはポート） |
| 起動の知らせ（`0x010` ＋ 種類の番号、DLC 0）を、受け持つ種類ごとに 1 本ずつ送る | C-36 | ○（MD だけ） |
| 起動の知らせは、CAN の初期化、保存した設定の読み込み、k の読み取りが済んでから送る。FB もその後から送り始める | C-37 | 両方（呼ぶ順はポート） |
| 起動の猶予: 起動の知らせから 200 ms は指令に従わない（[bus.md](bus.md) の 3.5） | C-89 | ○（MD だけ。ほかの種類はポートが `mas_can_node_in_grace` で見る） |
| 異常の詳細の bit 0（再起動した）を立てて起動する | C-67、C-69 | ○ |

### 指令と停止

| 守ること | 根拠 | node |
|---|---|---|
| 自分の slot が DLC の外なら、その ch の指令は受けていない扱い（途絶の時計は進み続ける） | C-42 | ○（MD） |
| ch ごとの指令の途絶は 100 ms。途絶したら出力を止める（止め方は [md.md](md.md) の 4.6）。ラッチしない | C-91、M-57、S-15 | ○（MD） |
| SYNC が途絶えても止めない。MD とサーボは SYNC の時刻を使わない | C-90、C-38 | ○（node は SYNC を見ない） |
| 指令の形が変わったら最後に受けた形に従い、前の形の制御の状態をリセットする | M-23 | 両方（知らせは node、リセットはポート） |
| 止めていた状態から動き出すときは、理由によらず制御の状態をリセットする | M-25 | 両方 |
| 範囲外の指令は基板の上限（設定またはハードウェア）で飽和させる。電流の上限に当たったら FB の bit 3 | M-28 | ポート |
| 電流の上限（項目 33）は duty を含むどの指令にも効かせる。電流を測れない基板での 33 の扱いは台帳では未定（[settings.md](settings.md) の 9） | M-47 | ポート（node は 33 をかけない） |
| 速度と位置の制御では、動きを最高速度（45）と最大加速度（46）の中に収める | M-29 | ポート |
| こなせない指令（電流を測れない基板への電流、エンコーダが無い基板への速度、duty を出せない基板への duty）は、受けたらすぐ止め（止め方は 34）、bit 4、bit 6–7 は受けた指令の形 | M-30、M-59 | ○（能力を cfg で渡す） |
| 位置の指令（`0x228`〜`0x22F`）は、今のファームでは実装しない。こなせない指令として扱う | M-55、M-49 | ○ |
| 保護で出力を止めている間は FB の bit 4。保護のやり方は基板に任せ、基板の資料に書く | M-31、C-7 | 両方（測るのはポート） |

### E-stop

| 守ること | 根拠 | node |
|---|---|---|
| `0x000` を受けたら DLC を問わず中身を見ず、種類ごとに決めた動作をしてラッチする | C-28 | ○（MD） |
| MD: 出力を止めてラッチする。止め方は常にブレーキ（34 によらない）。ブレーキを出せない基板は空転に丸める。bit 4 と bit 5 | M-51 | 両方（ブレーキを出すのはポート） |
| サーボ: パルスを止めてラッチする | S-9 | ポート |
| 解除は `0x001`、DLC 3 ちょうどで `"clr"` のときだけ | C-29 | ○ |
| ラッチは解除か電源の再投入で解ける | C-30 | ○ |
| MD: 解除の後は起動直後と同じ状態から（次の有効な指令から動く、制御の状態はリセット。[md.md](md.md) の 4.3） | M-27 | 両方 |

### FB（MD）

| 守ること | 根拠 | node |
|---|---|---|
| FB は `0x320` ＋ ch、ch ごとに 1 フレーム | M-14 | ○ |
| 指令が来ていなくても周期的に送る。何も測れない基板も送る。周期は項目 35 | M-21 | ○ |
| 中身と DLC（8、5、3）は測れるものに応じて決まる | M-15 | ○（能力を cfg で渡す） |
| 状態の bit（0 異常あり、1 速度と位置が無効、2 電流が無効、3 電流の上限、4 止めている、5 E-stop、6–7 指令の形） | M-18、C-45、M-20 | ○（bit 3 と保護はポートが渡す） |
| FB の duty は実際に出している値 | M-16 | ポート（値を渡す。測れるものによらず必ず渡す） |
| 電流と速度は ±32767 で飽和（−32768 は使わない） | M-17 | ○ |
| 量は出力軸で表す。位置は 1 回転 ＝ 65536 カウント、24 bit で折り返し、電源を入れたときは 0。速度は出力軸の rpm、電流はモーターの巻線の電流 | M-2、M-45 | ポート（測って換算する） |

### 設定

| 守ること | 根拠 | node |
|---|---|---|
| ID ＝ `0x400` ＋ 種類の番号 × 16 ＋ 向き × 8。DLC 8 だけ、下位 3 bit が 0 のものだけを受け、ほかは応答しない | C-46、C-47 | ○（MD） |
| 宛先の ch を受け持つ基板だけが応答する。受け持たない ch には何も返さない | C-49 | ○ |
| 操作（読む、書く、保存、初期化）、結果の番号と確かめる順、値の欄の決まり | C-48、C-52、C-85、C-56、C-57 | ○ |
| 共通の項目 0〜6 に答える | C-60 | ○（MD） |
| 書いた値が能力を超えるなら抑えて受け付け、抑えた値を返す。意味の上でありえない値は結果 3 | M-43、C-79 | ○（上限は cfg で渡す） |
| 機体の値は保存できる。プログラムの値（48、49）は保存せず、起動すると焼いた値に戻る | C-80、M-56 | ○ |

### 保存、初期化、再起動

| 守ること | 根拠 | node |
|---|---|---|
| 値は焼いた値 → 保存した値 → 主制御からの上書きの順で決まる | C-5 | ○ |
| 保存は保存できる項目だけを書く。初期化は保存した値を消し、保存できる項目だけを焼いた値に戻す | C-54 | ○ |
| 範囲（byte 2）は 0 ＝ その ch、1 ＝ 基板の全 ch。それ以外は 3 | C-53 | ○ |
| 動作中でも保存を書き、書き終えてから応答する | C-55 | 両方（フラッシュを書くのはポート） |
| 項目 6 は byte 3–5 が `"rst"` のときだけ、応答（成功）を送り終えてから再起動する。署名が違えば 3 | C-66 | 両方（線に出し終えるのを待つのはポート。`restart` は必須） |

### 異常の詳細（項目 5）とバスオフ

| 守ること | 根拠 | node |
|---|---|---|
| bit 0 再起動した、bit 1 保存した設定が壊れていたので焼いた値で起動した、bit 2 バスオフから復帰した | C-67 | ○（bit 2 はポートが知らせる） |
| MD の bit 8 過電流、9 過熱、10 電源電圧の低下、11 電源電圧が高すぎる、12 エンコーダの異常、13 ゲートドライバの異常 | M-44 | ポート（`mas_can_node_raise_fault`） |
| 立った bit は主制御が 1 を書いて消すまで残る。測る手段の無い基板はその bit を立てない | C-67 | ○ |
| どれかの bit が立っている間、FB の bit 0 を 1 | C-45 | ○ |
| バスオフからはマイコンの標準の自動復帰で戻り、復帰したら bit 2。バスオフの仕組みが無い実装（can2040）は立てない | C-68 | ポート |

## 基板の資料に書くこと

プロトコルは次を基板ごとの資料に任せている。README などに書く。

| 書くこと | 根拠 |
|---|---|
| k の決め方と bit 数 | C-75、C-82 |
| k が範囲外のときの知らせ方 | C-18 |
| 固有 ID にどの番号を使うか（C-62 に無いマイコン） | C-86 |
| ハードウェアの限界（電流など）と、項目 33 の既定 | C-79、M-47 |
| フラッシュに書いている間の出力（止まるなら、どう止まるか） | C-55、C-7 |
| 保護のやり方（異常に気づいたときの動作） | M-31、C-7 |
| サーボ: パルスの周期（34）を共有する PWM の組の分け方 | S-12 |

## 受信フィルタ

### MD の基板が受ける ID と送る ID

| 向き | ID | 中身 | 根拠 |
|---|---|---|---|
| 受ける | `0x000` | E-stop（リモートフレームも通す） | C-28、C-11 |
| 受ける | `0x001` | E-stop の解除 | C-29 |
| 受けてよい | `0x002` | SYNC。MD は使わない（node も見ない） | C-32、C-90 |
| 受ける | `0x210`〜`0x22F` | MD の指令（電流、速度、duty、位置）。自分の ch のフレームだけでもよい: `0x210` ＋ 周期指令の番号 × 8 ＋ ch ÷ 4 | M-6 |
| 受ける | `0x400` | MD の設定の要求 | C-46 |
| 送る | `0x010` | 起動の知らせ（MD） | C-36 |
| 送る | `0x320` ＋ ch | FB | M-14 |
| 送る | `0x408` | MD の設定の応答 | C-46 |

サーボも受け持つ基板は、`0x230`〜`0x237`（指令、S-1）と `0x410`（設定の要求）を受け、`0x011`（起動の知らせ）と `0x418`（設定の応答）を送る。ID の全体は [ids.md](ids.md)。

設定の要求の ch は node が見て、受け持たない ch には答えない（C-49）。フィルタで ch を絞らなくてよい。

### 実例（micro-MD、bxCAN 互換の 32 bit のマスク）

`firmware/micro-MD/src/main.c` の `can_filter`。標準 ID は上の 16 bit の bit 5〜15 に置く。下の 16 bit のマスクは IDE の bit（`0x0004`）だけで、RTR の bit を見ないので、リモートフレームの `0x000` も通る。ほかの ID のリモートフレームも通るので、ポートは `rtr` を埋めて node に渡す（node が捨てる）。

```c
static void can_filter(uint8_t bank, uint16_t id, uint16_t mask) {
    CAN_FilterInitTypeDef fl = {0};
    fl.CAN_FilterNumber = bank;
    fl.CAN_FilterMode = CAN_FilterMode_IdMask;
    fl.CAN_FilterScale = CAN_FilterScale_32bit;
    fl.CAN_FilterIdHigh = (uint16_t)(id << 5);
    fl.CAN_FilterMaskIdHigh = (uint16_t)(mask << 5);
    fl.CAN_FilterMaskIdLow = 0x0004u; /* 標準 ID だけ（リモートフレームも通す。0x000 のため） */
    fl.CAN_FilterFIFOAssignment = CAN_Filter_FIFO0;
    fl.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&fl);
}

/* can_init の中 */
    can_filter(0, 0x000, 0x7FC); /* E-stop、解除、SYNC */
    can_filter(1, 0x210, 0x7F0); /* MD の電流・速度の指令 */
    can_filter(2, 0x220, 0x7F0); /* MD の duty・位置の指令 */
    can_filter(3, 0x400, 0x7FF); /* MD の設定の要求 */
```

| bank | ID / マスク | 通る ID |
|---|---|---|
| 0 | `0x000` / `0x7FC` | `0x000`〜`0x003` |
| 1 | `0x210` / `0x7F0` | `0x210`〜`0x21F` |
| 2 | `0x220` / `0x7F0` | `0x220`〜`0x22F` |
| 3 | `0x400` / `0x7FF` | `0x400` だけ |

STM32 の HAL（`firmware/CAN_MD/App/app.c`）も同じ形で、欄の名前が `FilterIdHigh` などになる。ただし CAN_MD の bank 0 は `0x000` / `0x700` で class 0 全部（`0x000`〜`0x0FF`）を通し、micro-MD（`0x7FC`、`0x000`〜`0x003`）より広い。余分に通ったフレームは node が捨てる。can2040 にはハードウェアのフィルタが無いので、全部を node に渡してよい（node は関係ない ID を捨てる。node.c）。

## node を使う

### 分担

| node が自動でやること | ポート（基板のファーム）がやること |
|---|---|
| E-stop のラッチと解除、`0x000` のリモートフレーム | CAN の初期化（ビットタイミング、フィルタ）、送受信。受けたフレームの `rtr` を埋める |
| MD の指令の解読、DLC の外の slot、こなせない指令 | 時刻（µs）を渡す |
| 指令の途絶（100 ms） | k を読み、範囲を確かめる（C-18） |
| 起動の知らせ（MD）、起動の猶予（200 ms） | 固有 ID、基板の種類、ファームの版、能力を cfg で渡す |
| FB を項目 35 の周期で組み立てる | フラッシュの読み書き（`load`、`save`） |
| 設定への応答（共通の項目 0〜6、MD の項目 32〜41、45、46、48、49） | 再起動（応答を線に出し終えてから） |
| 書いた値の範囲の確かめと、能力の上限で抑える | モーターに出す（node の出力を受けて） |
| 保存と初期化（保存する中身を組み立て、CRC を付ける） | 電流の上限（duty を含むどの指令にも）、最高速度、最大加速度、速度ループの PID を効かせる |
| プログラムの値（48、49）を保存しない、起動で 1.0 に戻す | エンコーダの換算（36、37、38）。位置、速度、電流を測る |
| 回転方向の反転（32）を指令と FB の両方にかける | 電流の上限に当たったこと、保護で止めていることを渡す |
| 刻み（48、49）の換算 | バスオフからの復帰と、基板の異常（bit 8〜）を知らせる |
| 異常の詳細の bit 0、bit 1 と、FB の bit 0 | サーボなど、node が持たない種類の全部 |

### 呼ぶ順

1. 出力を止めた状態で用意する（M-22）。
2. k を読み、範囲を確かめる（C-18）。
3. `mas_can_node_config_t` を埋めて `mas_can_node_init`。ここで保存した設定を読む。
4. CAN を初期化する。
5. `mas_can_node_ready`。起動の知らせを積み、FB と起動の猶予が始まる（C-37、C-89）。
6. main ループで、受けたフレームを `mas_can_node_on_frame`、`mas_can_node_tick`、出力（`mas_can_md_output`）、測った値（`mas_can_md_set_measure`）、送る（`peek_tx`／`drop_tx`）。

### mas_can_node_config_t の欄

`mas_can_node_init` が中に写すので、呼んだ後に cfg を捨ててよい（node.c）。使わない欄は 0 にする（`memset` か `= {0}`）。

| 欄 | 意味 | 注意 |
|---|---|---|
| `uid` | 固有 ID（項目 0） | C-62、C-86 の作り方で |
| `board_type` | 項目 1 の値。`MAS_CAN_BOARD_TYPE(種類, 版)` | 登録の手順は上 |
| `fw_version` | 項目 2 の値。`0x00MMmmpp`（大、中、小） | C-70 |
| `md_first_ch` | 受け持つ MD の先頭の ch ＝ k × 基板の大きさ | 範囲はポートが確かめる（node は見ない） |
| `md_count` | 受け持つ MD の数（0〜`MAS_CAN_NODE_MD_MAX` ＝ 4） | 4 を超えると 4 に切る。0 なら MD の起動の知らせも送らない |
| `md_has_current` | 電流を測れる | false なら FB の bit 2、電流の指令はこなせない指令 |
| `md_has_speed` | 速度と位置を測れる | false なら FB の bit 1、速度の指令はこなせない指令 |
| `md_has_coast` | 空転を出せる | false なら 34 は常に 1（0 を書くと 1 に抑えて返す）。duty の bit 0 が空転でも `brake` は true |
| `md_no_duty` | duty を出せない（中継役など） | true なら duty の指令はこなせない指令（M-59）。0 で初期化すれば出せる扱い |
| `md_current_limit_max_ma` | 33（電流の上限）に書ける最大。ハードウェアの限界 | **0 のままだと、33 に書いた値は常に 0 に抑えられる**。抑えるのは書いたときだけで、焼いた値と保存した値はこの上限で抑えない。33 の焼いた値以上にする |
| `md_fb_rate_max_hz` | 35（FB の周期）に書ける最大 | **0 のままだと 35 を書いた時点で 0（送らない）に抑えられ、FB が止まる**。抑えるのは書いたときだけ（焼いた値 100 は抑えない）ので、100 以上にする |
| `md_current_limit_default_ma` | 33 の焼いた値 | M-47: ハードウェアの限界以下。0 のままだと 33 の焼いた値が 0 |
| `md_encoder_cpr_default` | 36 の焼いた値 | 0 なら 1 |
| `load` | 保存した設定を `buf` に `len` byte 読む。読めたら true | 下の「フラッシュ」 |
| `save` | `buf` の `len` byte をフラッシュに書く。書けたら true | false なら応答は結果 63 |
| `restart` | 項目 6 の再起動 | **必須**。NULL だと、項目 6 に成功を返したうえで再起動しない。下の「再起動」 |
| `ctx` | `load`、`save`、`restart` にそのまま渡す | 使わなければ NULL |

node が持つ焼いた値（基板で変えられないもの）: 32 ＝ 0、34 ＝ 0（空転を出せなければ 1）、35 ＝ 100、37 ＝ 1.0、38 ＝ 0、39〜41 ＝ 0、45、46 ＝ 0、48、49 ＝ 1.0（node.c）。

今の node では台帳の決まりに届かないところ:

- 39〜41（速度ループのゲイン）の焼いた値を基板ごとに渡す欄が無い。焼いた値は 0 になり、M-54 の「既定は基板の既定」にできない。
- 38 に結果 4（実現できない）を返す道が無い。向きが転流と一体の基板（BLDC）は、node のままでは M-41 を守れない。
- ブレーキを出せない基板を表す欄が無い。そういう基板（CAN-Servo-v1 の MD）でも 34 に 1 を書けて読むと 1 が返るが、実際は空転で出す（E-stop の丸めは M-51 の範囲）。

### 関数

| 関数 | いつ呼ぶ | すること |
|---|---|---|
| `mas_can_node_init(node, cfg, now)` | 起動時に 1 回。CAN を始める前 | cfg を写し、`load` で保存した設定を読み、異常の詳細の bit 0 を立てる |
| `mas_can_node_ready(node, now)` | CAN を初期化した後に 1 回 | MD の起動の知らせを積む。ここから FB を送り、200 ms の起動の猶予を数える |
| `mas_can_node_on_frame(node, f, now)` | 受けた標準 ID のフレームごと | `0x000`、`0x001`、`0x210`〜`0x22F`、`0x400` を処理する。ほかは捨てる。設定の応答と保存はこの中で行う |
| `mas_can_node_tick(node, now)` | main ループで毎回 | 指令の途絶を判定し、FB の時刻が来たら積む |
| `mas_can_node_peek_tx(node)` | 送るとき | 送る待ち行列の先頭（無ければ NULL） |
| `mas_can_node_drop_tx(node)` | 先頭を CAN のコントローラに渡せた後 | 先頭を消す。再起動の要求があり、待ち行列が空になったら `restart` を呼ぶ |
| `mas_can_md_output(node, local)` | ch ごとに、ループ 1 周に 1 回 | 出すべきものを返す（下）。`reset_ctrl` は読むと消える |
| `mas_can_md_set_measure(node, local, m)` | ch ごとに、ループ 1 周に 1 回 | 測った値を渡す。次の FB に載る |
| `mas_can_md_settings(node, local)` | 制御に設定を使うとき | 設定の今の値（`mas_can_md_settings_t`）。範囲外の local は NULL |
| `mas_can_node_raise_fault(node, bits)` | 基板が異常に気づいたとき | 異常の詳細の bit を立てる（バスオフは `MAS_CAN_FAULT_BUS_OFF`、MD の bit 8〜13 は `1u << 8` など） |
| `mas_can_node_in_grace(node, now)` | 指令を自前で受けるとき | `ready` の前と、その後 200 ms の間は true |

`local` は基板の中の番号（0〜`md_count` − 1）で、ch ＝ `md_first_ch` ＋ `local`。時刻はすべて µs の `uint32_t` で、一周してよい（node.h）。

`mas_can_node_t` の中の欄は直接触らない（node.h）。`static` に 1 つ置く（node.h が中身を公開しているのはそのため）。ただし、E-stop のラッチ、起動の知らせの後か、異常の詳細、cfg を読む関数は無い。今は `node.estop`、`node.ready`、`node.cfg` は読んでよい（書かない）。既存のファームも読んでいる（CAN-Servo-v1 は `node.estop` と `node.ready`、CAN_MD と micro-MD は `node.cfg.md_first_ch`）。

### 出力（mas_can_md_output_t）

| `mode` | `value` | `brake` |
|---|---|---|
| `MAS_CAN_MD_OUT_STOP` | 0 | true ＝ ブレーキ（短絡）、false ＝ 空転。E-stop では常に true（M-51） |
| `MAS_CAN_MD_OUT_DUTY` | duty（−1.0〜＋1.0） | オフの間をブレーキにするか（duty の bit 0。空転を出せない基板は常に true） |
| `MAS_CAN_MD_OUT_CURRENT` | 電流 [mA] | — |
| `MAS_CAN_MD_OUT_SPEED` | 速度 [rpm]（出力軸） | — |

- 値は刻み（48、49）と回転方向の反転（32）をかけた後のもの。ポートはそのまま使う。
- 途絶、E-stop、起動の猶予、こなせない指令、保護（`protecting`）、位置の指令は、すべて `STOP` になる。
- `reset_ctrl` が true なら、制御の状態（積分項、加速度の制限の起点など）をリセットする（M-23、M-25）。
- 電流の上限（33）、最高速度（45）、最大加速度（46）、速度ループの PID（39〜41）は node はかけない。`mas_can_md_settings` の値を使ってポートが効かせる（M-28、M-29、M-32）。33 は duty の指令にも効かせる（M-47）。

### 測った値（mas_can_md_measure_t）

| 欄 | 中身 |
|---|---|
| `current_ma` | 電流 [mA]（巻線の電流、M-45）。`md_has_current` のときだけ意味がある |
| `speed_rpm` | 出力軸の速度 [rpm]。`md_has_speed` のときだけ |
| `position` | 出力軸の位置。1 回転 ＝ 65536、24 bit で折り返す。電源投入で 0（M-2） |
| `duty` | 実際に出している duty（−1.0〜＋1.0、M-16） |
| `current_limited` | 電流の上限に当たっている（FB の bit 3、M-28） |
| `protecting` | 基板の保護で止めている（FB の bit 4、M-31）。true の間、出力は `STOP` |

- 符号は基板から見た向き（エンコーダの反転 38 をかけた後、回転方向の反転 32 をかける前）。32 は node が FB にかける（M-41）。
- エンコーダのカウントから出力軸の位置と速度への換算（36、37）はポートが行う（M-3）。
- 測れない欄（電流、速度、位置）は 0 のままでよい。`duty` は測れるものによらず必ず入れる（M-16。電流か速度を測れない基板では FB に載る）。

### フラッシュ（load と save）

- 中身は `mas_can_node_saved_t`（今は 172 byte）。`len` はいつもこの大きさ。ポートは中身を解釈しない。
- `load`: 読めたら true。node が印と CRC を確かめる。印が無い（消したままのフラッシュなど）ときは黙って焼いた値で起動する。印があって CRC か中身の形の版が合わないときは、異常の詳細の bit 1 を立てて焼いた値で起動する（node.c、C-67）。
- `save`: 書き終えてから返す（C-55）。保存と初期化の要求のたびに、全 ch の分を丸ごと書く（node.h）。
- 消去や書き込みの間に CPU が止まるマイコンでは、その間は出力を変えられない。どうするかは基板に任せ、基板の資料に書く（C-7）。既存のファームは分かれる: CAN_MD（`App/app.c` の `flash_save`）と micro-MD（`flash_write_page`）は書く前にブレーキにし、CAN-Servo-v1（`settingsSave`）は全部の出力を Low にし、`examples/node_md_rp2040`（`settingsSave`）は 2 本の入力を 0 にする（空転）。

### 再起動（restart）

- 項目 6 に `"rst"` を書くと、node は成功の応答を積み、待ち行列が空になった `drop_tx` の中で `restart(ctx)` を呼ぶ（node.c）。
- この時点で応答はコントローラに渡っただけで、線に出たとは限らない。応答を線に出し終えるのを待ってから再起動するのはポート（node.h、C-66）。例: `firmware/micro-MD/src/main.c` の `restart`（送信の箱が空くまで最大 10 ms 待つ）、`firmware/CAN-Servo-v1/src/main.cpp` と `examples/node_md_rp2040/src/main.cpp` の `restart`（送り終えの通知を数え、最大 10 ms 待つ）。CAN_MD は待たない（下の「既存のファーム」）。
- `restart` を NULL にすると、node は項目 6 に成功を返したうえで再起動しない。必ず渡す。

### main ループの雛形

1 ch の MD。`port_` で始まる関数は基板ごとに書く。node と core と一緒に gcc でコンパイルできることを確かめてある。

```c
#include <string.h>

#include "mas/can/node.h"

#define BOARD_SIZE 1           /* 受け持つ MD の ch の数（基板の大きさ） */
#define FW_VERSION 0x00000100u /* 項目 2: 0x00 大 中 小（C-70）。0.1.0 */
#define CURRENT_MAX_MA 10000u  /* ハードウェアの限界（C-79。基板の資料に書く） */

/* ---- ポートが用意する関数（基板ごとに書く） ---- */
uint32_t port_now_us(void);                   /* 1 µs 刻みの時計。一周してよい */
uint8_t port_read_k(void);                    /* 基板の位置 k（C-75） */
uint64_t port_unique_id(void);                /* C-62、C-86 */
void port_motor_init(void);                   /* 出力を止めた状態で用意する（M-22） */
void port_show_bad_k(void);                   /* k が範囲外の知らせ方（基板の資料に書く） */
void port_can_init(void);                     /* 1 Mbps、受信フィルタ */
bool port_can_receive(mas_can_frame_t* f);    /* 受けたフレームを 1 本取り出す（rtr も埋める）。無ければ false */
bool port_can_send(const mas_can_frame_t* f); /* 送信の箱に入れられたら true */
bool port_can_bus_off_recovered(void);        /* バスオフから戻った直後に 1 回だけ true */
void port_motor_apply(uint8_t i, const mas_can_md_output_t* out, const mas_can_md_settings_t* set);
void port_motor_measure(uint8_t i, mas_can_md_measure_t* m);
bool port_flash_read(void* ctx, void* buf, size_t len);
bool port_flash_write(void* ctx, const void* buf, size_t len);
void port_restart(void* ctx);

static mas_can_node_t node;

int main(void) {
    port_motor_init();

    /* k は起動時に 1 回だけ読む。範囲外なら CAN に何も送らず受けず、出力を止めたまま（C-18） */
    uint8_t k = port_read_k();
    unsigned first = (unsigned)k * BOARD_SIZE; /* C-81 */
    if (first + BOARD_SIZE > MAS_CAN_MD_MAX_CH) {
        for (;;) port_show_bad_k();
    }

    mas_can_node_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.uid = port_unique_id();
    cfg.board_type = MAS_CAN_BOARD_TYPE(MAS_CAN_BOARD_RESERVED, 1); /* 登録した番号に替える */
    cfg.fw_version = FW_VERSION;
    cfg.md_first_ch = (uint8_t)first;
    cfg.md_count = BOARD_SIZE;
    cfg.md_has_current = true;
    cfg.md_has_speed = false;
    cfg.md_has_coast = false;
    cfg.md_current_limit_max_ma = CURRENT_MAX_MA;
    cfg.md_current_limit_default_ma = CURRENT_MAX_MA;
    cfg.md_fb_rate_max_hz = 500;
    cfg.load = port_flash_read;
    cfg.save = port_flash_write;
    cfg.restart = port_restart;
    mas_can_node_init(&node, &cfg, port_now_us()); /* 保存した設定を読む */

    port_can_init();
    mas_can_node_ready(&node, port_now_us()); /* 起動の知らせ。ここから FB と起動の猶予 */

    for (;;) {
        uint32_t now = port_now_us();

        mas_can_frame_t rx;
        while (port_can_receive(&rx)) mas_can_node_on_frame(&node, &rx, now);
        if (port_can_bus_off_recovered()) mas_can_node_raise_fault(&node, MAS_CAN_FAULT_BUS_OFF);
        mas_can_node_tick(&node, now);

        for (uint8_t i = 0; i < BOARD_SIZE; i++) {
            mas_can_md_output_t out = mas_can_md_output(&node, i); /* 1 周に 1 回だけ呼ぶ */
            port_motor_apply(i, &out, mas_can_md_settings(&node, i));

            mas_can_md_measure_t m;
            memset(&m, 0, sizeof(m));
            port_motor_measure(i, &m);
            mas_can_md_set_measure(&node, i, &m);
        }

        const mas_can_frame_t* f;
        while ((f = mas_can_node_peek_tx(&node)) && port_can_send(f)) mas_can_node_drop_tx(&node);
    }
}
```

### 気をつけること

| 点 | 中身 |
|---|---|
| 受信は割り込みで積む | 割り込みで `mas_can_frame_t` の輪に積み、main ループで `on_frame` に渡す（既存のファームはすべてこの形）。拡張 ID は積まない。`rtr` を埋める |
| E-stop の早さ | node が止めるのは次の `mas_can_md_output` から。ループが遅いとその分遅れる。CAN-Servo-v1 は受信の割り込みの中で `0x000` を見て、すぐ出力を切っている |
| ループの速さ | 途絶（100 ms）と FB の周期（35）は `tick` を呼んだ時刻で数える。FB の周期よりずっと速く回す |
| 送る待ち行列 | 長さ 8（`MAS_CAN_NODE_TXQ`）。溢れた分は捨てる。捨てられるのは FB だけではなく、設定の応答、起動の知らせ、再起動の応答も同じ（FB は次の周期でまた積まれるが、ほかは失われる。node.c）。毎周、送れるだけ送る |
| `mas_can_md_output` を 1 周に 2 回呼ばない | `reset_ctrl` は読むと消える |
| 時計が粗いとき | ms の時計 × 1000 でも動く（`firmware/CAN_MD` は `HAL_GetTick() * 1000`）。FB の周期はその刻みに丸まる |
| node を更新したとき | 保存の中身の形が変わると、前の保存した設定は使われず、異常の詳細の bit 1 が立つ（上の「フラッシュ」） |

## ビルドへの組み込み

要るのは次の 3 つの C ファイルと、インクルードの道（C11）。

| ファイル | 中身 |
|---|---|
| `lib/mas/src/can/node.c` | node |
| `lib/mas/src/can/core/md_frame.c` | MD の指令と FB の形 |
| `lib/mas/src/can/core/setting.c` | 設定と class 0 の形 |
| `lib/mas/include` | インクルードの道（`#include "mas/can/node.h"`） |

`lib/mas/library.json` は Arduino（ESP32、RP2040）向けで、主制御の C++ を含む。Arduino でない基板（WCH の SDK、STM32 の CMake など）は `lib_deps` でライブラリごと取り込まず、上の 3 つだけを足す。

### PlatformIO（Arduino でない）: micro-MD

`firmware/micro-MD/platformio.ini`:

```ini
build_flags =
    -I ../../lib/mas/include
extra_scripts = mas_node.py
```

`firmware/micro-MD/mas_node.py`:

```python
Import("env")

env.Prepend(
    LIBS=[
        env.BuildLibrary(
            "$BUILD_DIR/mas",
            "$PROJECT_DIR/../../lib/mas/src/can",
            src_filter=["-<*>", "+<node.c>", "+<core/md_frame.c>", "+<core/setting.c>"],
        )
    ]
)
```

道はリポジトリの中の置き場所からの相対。別の場所に置くなら `../../lib/mas` を直す。

### PlatformIO（Arduino）: RP2040

`examples/node_md_rp2040/platformio.ini` と `firmware/CAN-Servo-v1/platformio.ini` は `lib_extra_dirs = ../../lib` で `lib/mas` を見せ、`#include "mas/can/node.h"` で取り込んでいる。CAN は `lib_deps` の ACAN2040（can2040）。

### CMake: CAN_MD

`firmware/CAN_MD/CMakeLists.txt`:

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    App/app.c
    ../../lib/mas/src/can/core/md_frame.c
    ../../lib/mas/src/can/core/setting.c
    ../../lib/mas/src/can/node.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App
    ../../lib/mas/include
)
```

CubeMX の生成物は触らず、`App/` にポートを置き、`main.c` の USER CODE から `app_init`、`app_loop` を呼ぶ形にしている。

## node が持たない種類（サーボなど）

node が受け持つのは MD（種類の番号 0）だけ（node.h）。サーボなどを受け持つ基板は、その種類の分をポートで書く。

| やること | 根拠 |
|---|---|
| その種類の起動の知らせ（サーボは `0x011`）を送る。MD の分は node が送る | C-36、C-37 |
| 起動の猶予の間は指令に従わない。`mas_can_node_in_grace` で見る | C-89 |
| E-stop でラッチしている間は指令に従わない | C-28、S-9 |
| DLC の外の slot は受けていない扱い。予約の値（サーボの 201〜254）は止める値と同じ | C-42、S-3 |
| ch ごとの途絶（100 ms）で止める | C-91、S-15 |
| その種類の設定（サーボは要求 `0x410`、応答 `0x418`）に、共通の項目と種類ごとの項目で答える | C-46、C-60、S-11 |

`firmware/CAN-Servo-v1/src/main.cpp` の `servoReceive`、`servoUpdate` と、`loop` の中のサーボの起動の知らせが実例。ただしこのファームは、サーボの設定の要求（`0x410`）に、共通の項目 0〜6 を含めて一切答えない（上の表の最後の行を守っていない。[servo.md](servo.md) の 4）。

## 既存のファーム

| 場所 | マイコンと作り | k | 特徴 |
|---|---|---|---|
| `firmware/CAN_MD` | STM32F042、HAL（CubeMX）、CMake。**Release** でビルドする（Debug は FLASH に入らない） | DIP 3 本 ＋ ビルドでずらす値（`-D CAN_MD_FIRST_CH=n`） | MD 1 ch、duty と電流。ポートは `App/app.c` |
| `firmware/micro-MD` | CH32V203K8T6、WCH の SDK（noneos-sdk）、PlatformIO | LED とスイッチで決めてフラッシュに覚える | MD 1 ch、duty と電流。`mas_node.py` で node だけを足す。バスオフの復帰の見張り（`watch_bus_off`）と、送り終えを待つ再起動がある |
| `examples/node_md_rp2040` | RP2040、Arduino、can2040、PlatformIO | ビルドで渡す（`-D MD_CH=0`） | 新しい基板の雛形。MD 1 ch、duty だけ。基板の種類は `0x0000` のまま |
| `firmware/CAN-Servo-v1` | RP2040-Zero、Arduino、can2040、PlatformIO | ビルドで渡す（`SERVO_FIRST_CH`、`MD_FIRST_CH`） | サーボ 8 ch ＋ MD 4 ch（duty だけ）。MD は node、サーボは自前（上の節）。送り終えを待つ再起動（最大 10 ms）がある |

can2040 はバスオフの仕組みを持たないので、RP2040 の 2 つは異常の詳細の bit 2 を立てない（C-68）。

### 台帳どおりでない点

既存のファームのうち、台帳の決まり（推奨を含む）と違うところ。新しい基板に写すときは真似しない。

| ファーム | 点 | 台帳 |
|---|---|---|
| `firmware/CAN_MD` | サンプル点が 75%（16 MHz、1 ＋ BS1 11 tq ＋ BS2 4 tq） | C-9（80%） |
| `firmware/CAN_MD` | 内蔵の RC 発振（HSI）から PLL で 16 MHz を作っている | C-10（水晶を推奨） |
| `firmware/CAN_MD` | `restart` ですぐ `NVIC_SystemReset` し、応答を線に出し終えるのを待たない | C-66 |
| `firmware/CAN_MD` | バスオフの自動復帰（`AutoBusOff`）は有効だが、復帰を見張らず、異常の詳細の bit 2 を立てない | C-68 |
| `firmware/CAN-Servo-v1` | サーボの設定の要求（`0x410`）に、共通の項目 0〜6 を含めて答えない | C-49、C-60、S-11 |
| `firmware/CAN-Servo-v1` | サーボの先頭の ch と MD の先頭の ch を、2 つのビルドの値（`SERVO_FIRST_CH`、`MD_FIRST_CH`）で別々に決める。k で主な種類の先頭だけを決め、他の種類はファームの規則で計算する形ではない | C-81 |

応答を送り終えてから再起動するのは micro-MD、CAN-Servo-v1、雛形（`examples/node_md_rp2040`）の 3 つ（どれも最大 10 ms 待つ）。

## 試し方

### PC のテスト

```sh
make -C lib/mas/test
```

| テスト | 中身 |
|---|---|
| `test_node` | node だけ。偽のフラッシュで、起動と FB、指令、E-stop、設定、起動の猶予、保護、duty を出せない基板 |
| `test_roundtrip` | 主制御のライブラリと node を 1 つのプログラムでつなぐ |
| `test_core`、`test_host` | 線の上の形と、主制御のライブラリ |

ポートの中の判断（k の範囲など）を PC で確かめたいときは、`lib/mas/test/test_node.c` と同じく、node と core と一緒に gcc でコンパイルする。

```sh
gcc -std=c11 -I lib/mas/include my_test.c \
    lib/mas/src/can/node.c lib/mas/src/can/core/md_frame.c lib/mas/src/can/core/setting.c
```

### 実機

主制御（ESP32 か RP2040）に `examples/md_minimal` を書き、基板と CAN でつなぐ。

1. `examples/md_minimal/src/main.cpp` の `can.md(0)` を、基板の ch に合わせる。
2. `pio run -e esp32 -t upload`（RP2040 なら `-e pico`）。シリアル（115200）を開く。
3. `MD not ready` が出ず、`online duty +0.30` が流れればよい。`offline` なら FB が届いていない（ビットタイミング、フィルタ、`ready` を呼んだか、35 が 0 でないか）。

E-stop、電流の指令、異常の bit を消す操作は `examples/md_serial_test`（シリアルから 1 文字の命令）で試せる。

### 基板の情報を生のフレームで読む

USB-CAN などで直接読むときの例。ch 0 の MD に項目 1（基板の種類）を読む（C-46、C-48）。

| | ID | data（16 進） |
|---|---|---|
| 要求 | `0x400` | `00 00 01 00 00 00 00 01`（ch 0、読む、項目 1、値 0、通し番号 1） |
| 応答 | `0x408` | `00 00 01 00 03 00 01 01`（結果 0、値 `0x00030001` ＝ micro-MD、版 1） |

項目 0 を読むと、応答の data 8 byte がすべて固有 ID になる（C-61）。項目と値の形の全体は [settings.md](settings.md)。
