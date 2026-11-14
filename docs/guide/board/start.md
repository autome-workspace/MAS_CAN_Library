# 雛形から始める

| | |
|---|---|
| 読む人 | 基板のファームを作る部員 |
| 前提 | 作る基板のマイコンで、PWM や ADC を動かすファームを書ける |
| 読むとできること | 雛形（`examples/node_md_rp2040`）を自分の基板に合わせ、主制御から動かせる |

新しい基板のファームは、雛形を写して作り始めます。雛形は、RP2040 でモーター 1 個を回す、モータードライバー（MD）の基板のファームです。雛形がこなせる指令は duty だけです。電流と速度は測りません。

雛形のファイルは 2 つです。[`platformio.ini`](../../../examples/node_md_rp2040/platformio.ini) がビルドの指定で、[`src/main.cpp`](../../../examples/node_md_rp2040/src/main.cpp) がファームの全部です。

このページは長いので、引き方を先に書きます。

- **自分の基板で何を変えるかだけを知りたいときは、** [自分の基板に合わせて変える所](#自分の基板に合わせて変える所) の表から引いてください。
- **雛形と違う基板（モーターが 2 個以上、番号を DIP スイッチで決める、電流を測れない）は、** [雛形と違う基板](#雛形と違う基板) にあります。
- **関数と欄の一覧は、** [node の関数と欄](node.md) にあります。

## 先に知っておくこと

### ファームは 2 つの部分でできる

基板のファームは、ライブラリの基板側と、あなたが書く部分の 2 つでできます。ライブラリの基板側を **node** と呼びます。node は C で書かれていて、ハードウェアには触りません。

node が受け持つのは、決まりを守る所です。

- **指令を受け取る。** 主制御から届いたフレームを読み、どの ch に何を出すかを決めます。
- **途絶で止める。** 指令が 0.1 秒届かないと、node はその ch を止めます。主制御が止まったときに、モーターが回り続けないためです。
- **緊急停止（E-stop）。** 主制御が緊急停止を送ると、node は止めたままにします。解除が来た後も、次の指令が来るまでは動きません。
- **起動の猶予。** 起動を知らせてから 0.2 秒は、node は指令に従いません。主制御が、起動した基板に設定や緊急停止を送り直す時間を取るためです。
- **設定に答える。** 主制御からの設定の読み書き、保存、再起動の求めに、node が答えます。
- **FB を返す。** FB は、基板が主制御へ周期的に返す、状態と測った値です。node は、既定で 1 秒に 100 回 FB を組み立てます。

あなたが書くのは、**ハードウェアに触る所**です。CAN の送受信、時計、フラッシュへの保存、再起動、モーターへの出力、測ること、の 6 つです。

### メインのループで、ファームがすること

雛形の冒頭のコメントは、メインのループでファームがすることを 4 つに分けています。

1. 受けたフレームを `mas_can_node_on_frame` に渡す。
2. `mas_can_node_tick` で時間を進める。
3. `mas_can_md_output` で出すべき値を受け取り、ハードウェアに出す。
4. `mas_can_node_peek_tx` と `mas_can_node_drop_tx` で、node が積んだフレームを送る。

node は、「今、モーターに何を出すべきか」を返します。ファームは、その値を判断し直さずに、そのまま出します。途絶や緊急停止で止めるかどうかは、node がもう決めているからです。

### node が受け持つのは MD だけ

今の node が受け持つ種類は、モータードライバーだけです。サーボの基板では、指令の受け取り、途絶の判断、起動の知らせ、緊急停止、起動の猶予を、ファームが自分で書きます。実例は [`firmware/CAN-Servo-v1/src/main.cpp`](../../../firmware/CAN-Servo-v1/src/main.cpp) です。書くことの一覧は、[node の関数と欄](node.md#node-が持たない種類サーボなどを自分で書く) にあります。

## 用意するもの

| もの | 雛形での使い方 |
|---|---|
| RP2040 の基板 | `platformio.ini` は Raspberry Pi Pico（`board = pico`）を指定しています。CAN-Servo の基板（RP2040-Zero）も、同じ指定でビルドしています |
| CAN のトランシーバ | マイコンの信号を、CAN の線の信号に変える部品です。トランシーバの TX 側を GPIO 0 に、RX 側を GPIO 1 につなぎます |
| モーターのドライバ | 入力が 2 本のものです。2 本の入力を GPIO 10 と GPIO 11 につなぎます |
| 主制御 | 動いたかを確かめるのに使います。ESP32 か Raspberry Pi Pico に、用意してある例を書き込みます |
| PC | PlatformIO が入っているもの |

雛形は、ドライバの 2 本の入力を両方 0 にすると空転、両方 1 にするとブレーキになる、と想定しています。この対応は、ドライバの品番で違います。使うドライバのデータシートで確かめてください。

CAN の送受信は、ACAN2040 というライブラリ（中身は can2040）が、RP2040 の PIO の 1 番を使って行います。

## 雛形を動かす

1. **雛形のフォルダを写します。** `examples/node_md_rp2040` を丸ごと、`firmware/` の下に新しい名前で写します。`platformio.ini` の `lib_extra_dirs = ../../lib` は、2 段上の `lib` フォルダからライブラリを探します。`firmware/` の下も 2 段なので、この行を直さずに使えます。CAN-Servo のファームも同じ行です。
2. **番号（ch）を決めます。** `platformio.ini` の `-D MD_CH=0` の 0 が ch です。決め方は下の「番号（ch）を決める」にあります。
3. **ビルドします。** 写したフォルダで `pio run` を実行します。最後に `[SUCCESS]` と出れば成功です。初めてのビルドでは、PlatformIO が `platform` の行の取り込み先から、RP2040 用の道具を取ってきます。
4. **書き込みます。** 同じフォルダで `pio run -t upload` を実行します。書き込んでも、基板の上では何も起きません。雛形には、LED もシリアルの表示も無いからです。
5. **主制御から見ます。** 主制御に [`examples/md_serial_test`](../../../examples/md_serial_test/) を書き込み、シリアルモニタ（115200）を開きます。雛形のままなら、次の行が出ます。

```
[0] on  0.000 A duty +0.00 stop fault 0x0
```

`[0]` の 0 は、手順 2 で決めた ch です。別の番号にしたなら、シリアルから `ch 3` のように送って切り替えます。`on` は、基板の声が主制御に届いている、という意味です。`stop` は、指令をまだ送っていないので止まっている、という意味です。

ここまで出たら、モーターを回して確かめます。手順は [確かめ方](test.md) にあります。

### 番号（ch）を決める

雛形の ch は、ビルドのときに決まります。`platformio.ini` の `build_flags` で `MD_CH` を渡し、`main.cpp` がそれを使います。渡さないときは 0 です。雛形のこの部分です。

```cpp
#ifndef MD_CH
#define MD_CH 0
#endif
static_assert(MD_CH < MAS_CAN_MD_MAX_CH, "MD_CH: 0..31"); // 範囲の外は、ほかの種類の信号とぶつかる
```

- **ch は 0〜31 から選びます。** MD の ch は 32 個です。
- **32 以上を書くと、ビルドが止まります。** 最後の行が確かめています。範囲の外の ch は、ほかの種類の基板の信号とぶつかるからです。
- **同じ線の上で、ほかの基板と同じ ch にしないでください。** 重なりを見つける仕組みはありません。
- **自分の基板で DIP スイッチから読むときは、電源を入れたときに 1 回だけ読みます。** 読み方と、範囲の外になりうるかの考え方は、下の [スイッチから番号を読む](#スイッチから番号を読む) にあります。点検の項目は [守ること](rules.md#番号ch) にあります。
- **モーターを 2 個以上受け持つ基板では、ch はモーターごとに付きます。** 決め方は、下の [モーターが複数のときの番号](#モーターが複数のときの番号) にあります。

## 雛形を読む

`main.cpp` を上から、まとまりごとに読みます。各まとまりの最後に、自分の基板で変える所を書きます。

### ピンと PWM の周期

雛形のこの部分です。

```cpp
constexpr uint8_t kCanTx = 0, kCanRx = 1, kCanPio = 1;
constexpr uint8_t kPinA = 10, kPinB = 11;               // モータードライバの 2 入力
constexpr uint16_t kPwmTop = (uint16_t)(F_CPU / 20000UL - 1); // 約 20 kHz
```

- `kCanTx` と `kCanRx` は、トランシーバにつなぐピンです。`kCanPio` は、can2040 が使う PIO の番号です。
- `kPinA` と `kPinB` は、モーターのドライバの 2 本の入力です。
- `kPwmTop` は、PWM の 1 周期のカウント数です。式の `20000UL` が、周波数（Hz）です。

**変える所:** ピンの番号は、基板の配線に合わせます。モーターの 2 本は、同じ PWM のスライスに載るピンにしてください。雛形は、`kPinA` のスライスにだけ周期を設定しているからです。RP2040 では、偶数の GPIO と、その次の GPIO が同じスライスです（10 と 11 は、どちらもスライス 5）。PWM の周波数は、ドライバが受け付ける範囲に合わせます。

can2040 は、PIO の 1 番の 4 つのステートマシンを、予約せずに使います。PIO を使うほかのライブラリを足すと、そのライブラリが同じステートマシンを空きと見て使うことがあります。雛形は、`setup()` の中の `reserveCanPio()` で、先に予約しています。

### CAN の受け取りと送り出し

受信は割り込みで行い、受けたフレームを 32 本の輪に積みます。node に渡すのは、メインのループです。雛形のこの部分です。

```cpp
// ---- CAN: 受信は割り込みで積み、loop() で取り出す ----
constexpr uint8_t kRxq = 32;
mas_can_frame_t rxq[kRxq];
volatile uint8_t rxHead, rxTail;
volatile uint8_t txInFlight; // can2040 に渡して、まだ線に出し終えていない本数

void onCan(struct can2040*, uint32_t notify, struct can2040_msg* msg) {
    if (notify == CAN2040_NOTIFY_TX && txInFlight) txInFlight--;
    if (notify != CAN2040_NOTIFY_RX || (msg->id & CAN2040_ID_EFF)) return;
    if ((uint8_t)(rxHead - rxTail) >= kRxq) return;
    mas_can_frame_t& f = rxq[rxHead % kRxq];
    f.id = msg->id & 0x7FF;
    f.rtr = (msg->id & CAN2040_ID_RTR) != 0;
    f.dlc = msg->dlc > 8 ? 8 : msg->dlc;
    for (int i = 0; i < 8; i++) f.data[i] = msg->data[i];
    rxHead++;
}
```

- **拡張 ID のフレームは積みません**（`CAN2040_ID_EFF` の行）。node に渡すフレーム（`mas_can_frame_t`）には、標準 ID と拡張 ID を区別する欄が無いからです。
- **リモートフレームかどうかを、必ず `rtr` の欄に入れます。** 入れないと、node がリモートフレームを指令として受けてしまいます。
- **輪が満ちているときは、新しく受けたフレームを捨てます。**
- **送って、まだ線に出し終えていない本数を数えています**（`txInFlight`）。再起動の前に、返事が線に出たかを確かめるのに使います。

送るほうは `canSend()` です。can2040 がフレームを受け付けられないとき、`canSend()` は false を返します。そのフレームは node の待ち行列に残り、次の周で送り直されます。

**変える所:** RP2040 と can2040 を使うなら、ここは変えなくて構いません。ほかのマイコンでは、この 2 つの関数を書き直します（下の「RP2040 でないマイコン」）。

### 設定の保存と再起動

node は、主制御が保存を求めたときに `save` を、起動したときに `load` を、再起動を求めたときに `restart` を呼びます。この 3 つの関数を書くのは、ファームです。雛形のこの部分です。

```cpp
// ---- node に渡す関数: 保存した設定の読み書き、再起動 ----
bool settingsLoad(void*, void* buf, size_t len) {
    EEPROM.begin(sizeof(mas_can_node_saved_t));
    memcpy(buf, EEPROM.getDataPtr(), len);
    return true;
}
// フラッシュに書いている間は割り込みも loop() も止まり、途絶も E-stop も見られない。先に出力を切る。
// 出力は、次の loop() の motorApply が戻す。
bool settingsSave(void*, const void* buf, size_t len) {
    pwm_set_gpio_level(kPinA, 0);
    pwm_set_gpio_level(kPinB, 0);
    memcpy(EEPROM.getDataPtr(), buf, len);
    return EEPROM.commit();
}
// 応答を線に出し終えるのを少し待ってから再起動する
void restart(void*) {
    uint32_t t0 = micros();
    while (txInFlight && (uint32_t)(micros() - t0) < 10000u) {
    }
    rp2040.reboot();
}
```

- **保存する中身は、node が組み立てます。** 中身は `mas_can_node_saved_t` の 172 byte です。ファームは中身を解釈せずに、そのまま書き、そのまま読みます。雛形は、Arduino の `EEPROM` を使って、RP2040 のフラッシュに置きます。
- **`settingsLoad` は、いつも true を返します。** 何も書いていないフラッシュでも構いません。node が中身の印と CRC を確かめ、合わなければ焼いた値（ファームが持つ既定の値）で起動するからです。
- **node が `restart` を呼ぶのは、再起動の返事を送る待ち行列が空になったときです。**

雛形が気をつけている所が 2 つあります。

- **書く前に、出力を切っています。** RP2040 は、フラッシュを消して書いている間、割り込みもメインのループも止まります。その間は、途絶も緊急停止も見られません。そこで、書く前に 2 本の入力を 0 にします（空転）。出力は、次の周の `motorApply()` が戻します。自分の基板で、書いている間に出力がどうなるかは、基板の資料に書いてください。
- **再起動は、返事が線に出るのを待ってから行います。** node が `restart` を呼んだ時点では、返事は can2040 に渡っただけで、線に出たとは限りません。すぐ再起動すると、返事が主制御に届かないことがあります。雛形は、`txInFlight` が 0 になるのを、最大 10 ms 待ちます。

その下の `uniqueId()` は、基板ごとに違う番号（固有 ID）を作ります。RP2040 のフラッシュが持つ 64 bit の番号を、先頭の byte を上位にして並べます。RP2040 なら、変えなくて構いません。

### モーターに出す

`motorInit()` は、2 本の入力を両方 0 にしてから PWM を始めます。電源を入れた直後に、出力を止めておくためです。`motorApply()` は、node が返した値をピンに出し、実際に出した duty を返します。雛形のこの部分です。

```cpp
float motorApply(const mas_can_md_output_t& out) {
    float d = out.mode == MAS_CAN_MD_OUT_DUTY ? out.value : 0.0f;
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    uint16_t on = (uint16_t)((d < 0 ? -d : d) * kPwmTop);
    // 止める、または duty 0 で、ブレーキの指定があればブレーキ。
    // 回している間の brake（PWM を切っている間をブレーキにする）は、ドライバに合わせて書く。雛形は空転のまま
    if ((out.mode != MAS_CAN_MD_OUT_DUTY || d == 0.0f) && out.brake) {
        pwm_set_gpio_level(kPinA, kPwmTop + 1);
        pwm_set_gpio_level(kPinB, kPwmTop + 1);
    } else {
        pwm_set_gpio_level(kPinA, d > 0 ? on : 0);
        pwm_set_gpio_level(kPinB, d < 0 ? on : 0);
    }
    return d;
}
```

node が返す `mas_can_md_output_t` の `mode` ごとに、雛形は次のように出します。

| `mode` | 雛形が出すもの |
|---|---|
| `MAS_CAN_MD_OUT_DUTY` | `value`（−1.0〜+1.0）の大きさで、片方の入力に PWM を出します。正なら `kPinA`、負なら `kPinB` です |
| `MAS_CAN_MD_OUT_STOP` | `brake` が true なら両方 1（ブレーキ）、false なら両方 0（空転）です |
| `MAS_CAN_MD_OUT_CURRENT`、`MAS_CAN_MD_OUT_SPEED` | 雛形には来ません。雛形は電流も速度も測れないと node に伝えているので、node がこの 2 つの指令を「止める」に変えます |

雛形が見ていない欄が 2 つあります。

- **回している間の `brake` を扱っていません。** この `brake` は、PWM を切っている間をブレーキにするか、を表します。雛形は、回している間はいつも空転で出します。duty が 0 のときだけは、`brake` に従ってブレーキにします。
- **`reset_ctrl` を見ていません。** 雛形は、制御の状態（積分の値）を持たないからです。電流や速度を制御する基板では、`reset_ctrl` が立ったら制御の状態を消します。

**変える所:** `motorApply()` は、ドライバの入力の表に合わせて書き直します。回る向きが逆なら、`kPinA` と `kPinB` を入れ替えます。値の意味の詳細は、[node の関数と欄](node.md#出すべきものmas_can_md_output_t) の「出すべきもの」にあります。

### 起動の順（setup）

`setup()` は、出力を止め、node に基板の性質を渡し、CAN を始め、最後に起動を知らせます。雛形のこの部分です。

```cpp
void setup() {
    motorInit();

    mas_can_node_config_t cfg = {};
    cfg.uid = uniqueId();
    cfg.board_type = MAS_CAN_BOARD_TYPE(MAS_CAN_BOARD_RESERVED, 1); // 実際の基板は core/board.h に番号を足す
    cfg.fw_version = 0x00000100u;       // 0.1.0
    cfg.md_first_ch = MD_CH;
    cfg.md_count = 1;
    cfg.md_has_current = false; // 測れない量は false（FB の bit で知らせる）
    cfg.md_has_speed = false;
    cfg.md_has_coast = true;
    cfg.md_fb_rate_max_hz = 200;
    cfg.load = settingsLoad;
    cfg.save = settingsSave;
    cfg.restart = restart;
    mas_can_node_init(&node, &cfg, micros());

    can.begin();
    reserveCanPio();
    mas_can_node_ready(&node, micros()); // 起動の知らせ。ここから 200 ms は指令に従わない
}
```

順番には理由があります。

1. **`motorInit()`** で、まず出力を止めます。
2. **`mas_can_node_init()`** に、基板の性質（`cfg`）を渡します。node はこの中で `load` を呼び、保存した設定を読みます。
3. **`can.begin()`** で、CAN を始めます。続けて `reserveCanPio()` で、CAN が使う PIO を予約します。
4. **`mas_can_node_ready()`** で、主制御に「起動した」と知らせます。ここから node は FB を送り始め、0.2 秒の起動の猶予を数えます。

`mas_can_node_ready()` は、準備が全部済んでから呼んでください。早く呼ぶと、準備ができていないのに主制御が話しかけてきます。時刻は、どの関数にも `micros()`（µs）で渡します。

### node に渡す性質（mas_can_node_config_t）

`cfg = {}` で、全部の欄を 0 にしてから埋めています。雛形が入れている欄は次のとおりです。

| 欄 | 雛形の値 | 意味 |
|---|---|---|
| `uid` | `uniqueId()` | 基板ごとに違う番号（固有 ID） |
| `board_type` | `MAS_CAN_BOARD_TYPE(MAS_CAN_BOARD_RESERVED, 1)` | 基板の種類の番号と、基板の版（1 から） |
| `fw_version` | `0x00000100u` | ファームの版。上から `0x00`、大、中、小の順で、これは 0.1.0 |
| `md_first_ch` | `MD_CH` | 受け持つ MD の先頭の ch |
| `md_count` | `1` | 受け持つ MD の数。最大 4 |
| `md_has_current` | `false` | 電流を測れるか |
| `md_has_speed` | `false` | 速度と位置を測れるか |
| `md_has_coast` | `true` | 空転を出せるか |
| `md_fb_rate_max_hz` | `200` | 主制御が FB の周期を設定するときの上限（Hz） |
| `load`、`save` | `settingsLoad`、`settingsSave` | 保存した設定の読み書き |
| `restart` | `restart` | 再起動 |

雛形が入れていない欄は、0 のままです。

- **`md_no_duty` が 0 なので、duty を出せる基板として扱われます。**
- **`md_current_limit_max_ma` と `md_current_limit_default_ma` が 0 です。** そのため、電流の上限（主制御が設定で読み書きする値）は、いつも 0 になります。雛形は電流を測れないので、0 のままで構いません。理由は下の [電流を測れない基板の、電流の上限](#電流を測れない基板の電流の上限) にあります。電流を測る基板では、部品の限界を入れます。
- **`md_encoder_cpr_default` が 0 です。** エンコーダの 1 回転のカウント数の既定の値で、0 なら node は 1 にします。
- **`ctx` が 0 です。** node は、`load`、`save`、`restart` の最初の引数に NULL を渡します。

`md_fb_rate_max_hz` は、100 以上にしてください。主制御が書いた FB の周期は、この上限で抑えられます。0 のままだと、主制御が FB の周期を書いた時点で 0 に抑えられ、FB が止まります。各欄の詳しい意味と、0 のままにしたときの動きは、[node の関数と欄](node.md#基板の性質を渡すmas_can_node_config_t) にあります。

### メインのループ（loop）

`loop()` は、冒頭のコメントの 4 つを、番号の順に行います。雛形のこの部分です。

```cpp
void loop() {
    uint32_t now = micros();
    while (rxTail != rxHead) mas_can_node_on_frame(&node, &rxq[rxTail++ % kRxq], now); // 1
    mas_can_node_tick(&node, now);                                                    // 2

    mas_can_md_measure_t m = {}; // 3
    m.duty = motorApply(mas_can_md_output(&node, 0));
    mas_can_md_set_measure(&node, 0, &m);

    const mas_can_frame_t* f; // 4
    while ((f = mas_can_node_peek_tx(&node)) && canSend(f)) mas_can_node_drop_tx(&node);
}
```

1. **受ける。** 割り込みが輪に積んだフレームを、全部 node に渡します。
2. **時間を進める。** node は、途絶を判断し、FB を送る時刻が来ていれば FB を積みます。
3. **出して、測った値を渡す。** `mas_can_md_output(&node, 0)` の 0 は、基板の中のモーターの番号（0 から）です。返ってきた値を `motorApply()` で出し、実際に出した duty を `mas_can_md_set_measure()` で node に渡します。渡した値は、次の FB に載ります。
4. **送る。** node が積んだフレームを、送れるだけ送ります。送れたものだけ、`mas_can_node_drop_tx()` で消します。再起動は、この中で待ち行列が空になったときに起きます。

`mas_can_md_output()` は、1 周に 1 回だけ呼んでください。`reset_ctrl` は、読むと消えるからです。

`loop()` の中で、長く待たないでください。node は、途絶（0.1 秒）と FB の周期（既定で 0.01 秒）を、`mas_can_node_tick()` を呼んだ時刻で数えます。緊急停止で止まるのも、次の `mas_can_md_output()` からです。ループが遅いと、止まるのも FB もその分だけ遅れます。

### 測った値を渡す

雛形が node に渡すのは、実際に出している duty だけです。電流や速度を測れる基板は、`mas_can_md_measure_t` のほかの欄も埋めます。

| 欄 | 中身 |
|---|---|
| `duty` | 実際に出している duty（−1.0〜+1.0）。どの基板も、いつも渡します |
| `current_ma` | 電流（mA）。`md_has_current` が true のときだけ |
| `speed_rpm` | 出力軸の速度（rpm）。`md_has_speed` が true のときだけ |
| `position` | 出力軸の位置。1 回転を 65536 として数えます |
| `current_limited` | 電流の上限に当たっているか |
| `protecting` | 基板を守るために止めているか |

値の向きと換算は、[node の関数と欄](node.md#測った値mas_can_md_measure_t) の「測った値」にあります。

## 雛形と違う基板

雛形は、モーター 1 個、番号はビルドで渡す、電流は測らない、という基板です。ここでは、モーターが 2 個以上の基板、番号を DIP スイッチで決める基板、電流を測れない基板で、雛形から変える所を書きます。例として、「モーター 2 個、番号は DIP スイッチ 3 本、電流は測れない」基板を使います。

### モーターが複数のときの番号

ch は、基板ではなく、モーター 1 個ごとに付く番号です。モーターを 2 個受け持つ基板は、続いた 2 つの ch を受け持ちます。

**先頭の ch は、スイッチの値に、受け持つモーターの数を掛けた値です。** 2 個の基板なら、スイッチの値の 2 倍が先頭の ch で、その次の ch が 2 個目のモーターです。掛けるのは、スイッチの値が 1 違う基板どうしで、ch が重ならないためです。

| スイッチの値 | 受け持つ ch（モーター 2 個の基板） |
|---|---|
| 0 | 0 と 1 |
| 1 | 2 と 3 |
| 3 | 6 と 7 |
| 7 | 14 と 15 |

`cfg` には、先頭の ch を `md_first_ch` に、受け持つ数を `md_count` に入れます。スイッチの値が 3 なら、`md_first_ch` は 6、`md_count` は 2 です。主制御のプログラムからは、`can.md(6)` と `can.md(7)` の 2 つの MD に見えます。

仕様書では、このスイッチの値を **k**（基板の位置）と呼びます。決まりは、仕様書の [基板の位置 k と ch](../../spec/board.md#基板の位置-k-と-ch) にあります。

### スイッチから番号を読む

**番号は、電源を入れたときに 1 回だけ読み、`mas_can_node_init` を呼ぶ前に `md_first_ch` に入れます。** 動いている途中で読み直すと、スイッチに触れただけで、基板が別の ch の指令に従い始めるからです。

多くの基板は、スイッチを ON にすると、入力が GND につながる配線です。この配線では、入力をプルアップにして読み、0 が読めたら ON と数えます。自分の基板の回路図で、ON のときに何が読めるかを確かめてください。

STM32 での手本は、CAN_MD（`firmware/CAN_MD/App/app.c`）の `read_dip()` です。3 本のスイッチを、それぞれ 1 bit として足し合わせます。

```c
static uint8_t read_dip(void) { /* 印字 1 ＝ bit 0（PA5）、2 ＝ PB1、3 ＝ PA4。on で 0 */
    uint8_t k = 0;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_5) == GPIO_PIN_RESET) k |= 1;
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_RESET) k |= 2;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) == GPIO_PIN_RESET) k |= 4;
    return k;
}
```

入力のプルアップは CubeMX で設定していて、`Core/Src/main.c` の `MX_GPIO_Init()` に `GPIO_PULLUP` として書き出されています。CAN_MD は、読んだ値に、ビルドで渡す `CAN_MD_FIRST_CH` を足して ch にしています。

RP2040 でスイッチを読むコードの手本は、まだありません。上の考え方で書いてください。スイッチで番号を決めるなら、雛形の `platformio.ini` の `-D MD_CH=0` と、`main.cpp` の `MD_CH` は使わなくなります。

**範囲の外になりうるかを、先に数えます。** MD の ch は 0〜31 です。基板が受け持つ最後の ch が 31 を超えると、ほかの種類の基板の信号とぶつかります。スイッチ 3 本の値は 0〜7 なので、モーター 2 個の基板では、先頭の ch は最大で 14 です。受け持つのは ch 14 と 15 までで、範囲に収まります。このように、スイッチの値が最大のときでも収まるなら、範囲の外の扱いは要りません。

収まらない組み合わせ（例: スイッチ 4 本で、モーター 4 個の基板）では、範囲の外のときに、CAN に何も送らず、何も受けず、出力を止めたままにします。そのことを LED で知らせ、知らせ方を基板の資料に書きます。点検の項目は [守ること](rules.md#番号ch) にあります。

決まりは、スイッチの本数を、32 個の ch を全部指せるだけにすることを勧めています。モーター 2 個の基板なら、16 通りの 4 本です。3 本の基板は ch 0〜15 しか受け持てませんが、決まりに反してはいません。

### モーターが複数のときの loop()

雛形の `loop()` の 3（出して、測った値を渡す）を、基板の中のモーターの番号ごとに回します。番号は 0 から `md_count` − 1 までで、そのモーターの ch は、`md_first_ch` に番号を足した値です。

手本は、MD を 4 個受け持つ CAN-Servo のファーム（`firmware/CAN-Servo-v1/src/main.cpp`）です。ピンは、モーターごとに 2 本の組を並べた表にしています。

```cpp
constexpr uint8_t kMotorPins[4][2] = {{10, 11}, {8, 9}, {6, 7}, {4, 5}};
```

`motorUpdate(i)` が、モーター `i` の 1 個分です。雛形の `loop()` の 3 にあたります。

```cpp
void motorUpdate(uint8_t i) {
    mas_can_md_output_t out = mas_can_md_output(&node, i);
    float duty = out.mode == MAS_CAN_MD_OUT_DUTY ? out.value : 0.0f; // 反転（32）は node がかけ済み
    if (duty > 1.0f) duty = 1.0f;
    if (duty < -1.0f) duty = -1.0f;
    uint16_t level = (uint16_t)((duty < 0 ? -duty : duty) * kPwmTop);
    uint8_t a = kMotorPins[i][0], b = kMotorPins[i][1];
    gpio_set_function(a, GPIO_FUNC_PWM);
    gpio_set_function(b, GPIO_FUNC_PWM);
    pwm_set_gpio_level(a, 0);
    pwm_set_gpio_level(b, 0);
    if (duty > 0) pwm_set_gpio_level(a, level);
    if (duty < 0) pwm_set_gpio_level(b, level);
    appliedDuty[i] = duty;

    mas_can_md_measure_t m = {};
    m.duty = appliedDuty[i];
    mas_can_md_set_measure(&node, i, &m);
}
```

`loop()` は、これを 4 個分回します。

```cpp
    for (uint8_t i = 0; i < 4; i++) motorUpdate(i);
```

- **`mas_can_md_output(&node, i)` と `mas_can_md_set_measure(&node, i, &m)` は、同じ `i` で、1 周に 1 回ずつ呼びます。** `i` が違えば、別のモーターです。
- **雛形をこの形にするには、** ピンを `kMotorPins` のような表にし、`motorApply()` にモーターの番号 `i` を渡し、`loop()` の 3 を番号ごとに回します。土台にするのは雛形の `motorApply()` です。中の `kPinA` を `kMotorPins[i][0]` に、`kPinB` を `kMotorPins[i][1]` に置き換えれば、ブレーキの分岐はそのまま使えます。
- **保存の間に出力を切る `settingsSave()` も、全部のモーターのピンを切るようにします。** 雛形は `kPinA` と `kPinB` しか切っていません。CAN-Servo のファームは、`forceOutputsLow()` で全部のピンを Low にしています。
- **CAN-Servo のファームは、ブレーキを使いません。** 止めるときも、いつも空転で出します。使っているドライバ（AM2837）のブレーキを、データシートで確かめていないからです。ブレーキを出せる基板では、雛形の `motorApply()` のように `brake` を見てください。

### 2 個目のモーターの PWM

雛形の `motorInit()` は、`kPinA` が載る PWM のスライスにだけ、周期を設定して動かしています（`pwm_set_wrap` と `pwm_set_enabled`）。2 個目のモーターのピンが別のスライスに載るなら、そのスライスにも周期を設定して、動かす必要があります。設定しないと、2 個目のモーターには、雛形が意図した PWM が出ません。

RP2040 では、偶数の GPIO と、その次の GPIO が同じスライスです。CAN-Servo のファームの 4 組（10 と 11、8 と 9、6 と 7、4 と 5）は、それぞれ別のスライスに載っています。CAN-Servo のファームの `motorPwmInit()` は、組ごとにスライスを調べて設定しています。

```cpp
void motorPwmInit() {
    for (const auto &pair : kMotorPins) {
        uint slice = pwm_gpio_to_slice_num(pair[0]);
        pwm_set_enabled(slice, false);
        pwm_set_clkdiv(slice, 1.0f);
        pwm_set_wrap(slice, kPwmTop);
        pwm_set_gpio_level(pair[0], 0);
        pwm_set_gpio_level(pair[1], 0);
        gpio_set_function(pair[0], GPIO_FUNC_PWM);
        gpio_set_function(pair[1], GPIO_FUNC_PWM);
        pwm_set_enabled(slice, true);
    }
}
```

1 個のモーターの 2 本は、これまでどおり、同じスライスに載るピンにします。

### 電流を測れない基板の、電流の上限

**電流を測れない基板では、`md_current_limit_max_ma` と `md_current_limit_default_ma` を 0 のままにして構いません。** node は、電流の上限（設定の項目 33）を、出力に使わないからです。0 のままでも、モーターは duty の指令どおりに回ります。

0 のままにしたときに変わるのは、主制御から見える値だけです。主制御が項目 33 を読むと 0 が返り、何を書いても 0 に抑えて返ります。CAN-Servo のファームは 2 つの欄に 0 を入れ、雛形は入れずに 0 のままにしています。

電流の上限を効かせるのは、ファームの仕事です。電流を測れない基板は、上限を効かせられません。電流を測れない基板で項目 33 が何を意味するかは、決まりではまだ決まっていません。基板の資料には、「電流を測れないので、電流の上限は効かない」と書いてください。

## 自分の基板に合わせて変える所

| 変える所 | 雛形 | 変え方 |
|---|---|---|
| CAN のピン | `kCanTx = 0`、`kCanRx = 1`、`kCanPio = 1` | 基板の配線に合わせます |
| モーターのピン | `kPinA = 10`、`kPinB = 11` | 同じ PWM のスライスに載る 2 本にします |
| PWM の周波数 | 約 20 kHz（`kPwmTop` の `20000UL`） | ドライバが受け付ける範囲にします |
| 出し方 | `motorApply()` | ドライバの入力の表に合わせて書き直します。回している間の `brake` も扱います |
| 番号（ch） | ビルドで渡す（`-D MD_CH=0`） | DIP スイッチで決めるなら、起動時に 1 回読み、範囲を確かめてから `md_first_ch` に入れます（[スイッチから番号を読む](#スイッチから番号を読む)） |
| 受け持つモーターの数 | `md_count = 1` | 最大 4 です。先頭の ch は、スイッチの値に数を掛けた値にします（[モーターが複数のときの番号](#モーターが複数のときの番号)）。`loop()` の 3 をモーターの番号ごとに回し（[モーターが複数のときの loop()](#モーターが複数のときの-loop)）、2 個目のモーターの PWM も設定します（[2 個目のモーターの PWM](#2-個目のモーターの-pwm)） |
| こなせる指令 | `md_has_current = false`、`md_has_speed = false`、`md_has_coast = true` | 測れるものを true にします。true にしたら、`motorApply()` で `MAS_CAN_MD_OUT_CURRENT` や `MAS_CAN_MD_OUT_SPEED` を出し、`reset_ctrl` を扱います。空転を出せないなら `md_has_coast` を false にします |
| 電流の上限 | 入れていない（0） | 電流を測る基板は、`md_current_limit_max_ma` と `md_current_limit_default_ma` に部品の限界を入れます。測れない基板は 0 のままで構いません（[電流を測れない基板の、電流の上限](#電流を測れない基板の電流の上限)） |
| 基板の種類の番号 | `MAS_CAN_BOARD_RESERVED`、版 1 | 番号は一覧に書き足して取るので、作者に頼みます。取るまでは、このままにします |
| ファームの版 | `0x00000100u`（0.1.0） | ファームを直したら上げます |
| 保存の間の出力 | 書く前に出力を切る（空転） | ブレーキで止めたい基板は、書き換えます |
| 再起動 | 返事が線に出るのを最大 10 ms 待つ | RP2040 と can2040 なら、変えなくて構いません |

基板の種類の番号の取り方は、仕様書の [基板の種類の登録の手順](../../spec/board.md#基板の種類の登録の手順) にあります。

## 次にすること

- **動いたかを確かめる。** 手順は [確かめ方](test.md) にあります。雛形は電流を測れないので、電流の指令（`i 0.5`）では回らずに止まるのが、正しい動きです。
- **守ることを点検する。** 自分のファームが決まりを満たしているかは、[守ること](rules.md) の点検表で確かめます。
- **基板の資料を書く。** 基板のフォルダの README に書くことは、[守ること](rules.md#基板の資料に書くこと) の最後にあります。
- **node の関数と欄を引く。** 関数ごとの呼ぶ時期と呼ぶ順、`mas_can_node_config_t` の全部の欄は、[node の関数と欄](node.md) にあります。

## RP2040 でないマイコン

RP2040 でないマイコンでは、node をビルドに足す方法と、CAN の送受信、時刻、フラッシュ、固有 ID、再起動の書き方が変わります。今ある 3 通りのやり方と、手本の関数は、[マイコンごとの組み込み](ports.md) にあります。今の手本は、次の 2 つのファームです。

- **STM32（STM32F042）:** [`firmware/CAN_MD`](../../../firmware/CAN_MD/)。ハードウェアに触る所は `App/app.c` にあり、`CMakeLists.txt` で node をビルドに足しています。
- **CH32（CH32V203）:** [`firmware/micro-MD`](../../../firmware/micro-MD/)。`mas_node.py` で、node だけを PlatformIO のビルドに足しています。

## このページにまだ書いていないこと

- **初めて書き込むときの、RP2040 の側の操作。** リポジトリの中に、確かめられる記録がありません。
- **PWM の正確な周波数。** `kPwmTop` は `F_CPU` から計算しますが、この環境での `F_CPU` の値を確かめていません。
- **RP2040 で DIP スイッチを読むコード。** 手本になるファームがなく、ビルドで確かめられないので、コードは載せていません。考え方は [スイッチから番号を読む](#スイッチから番号を読む) にあります。STM32 での例は、`firmware/CAN_MD/App/app.c` の `read_dip()` です。
- **番号が範囲の外のときの扱いのコード。** 既存のファームには、起動したときに範囲の外を見つけて止まる例がありません。
- **電流や速度を測って制御する書き方。** 電流の例は、`firmware/CAN_MD` と `firmware/micro-MD` です。
- **基板が見つけた異常（過電流、過熱）を node に知らせる書き方。** 雛形にはありません。使う関数は `mas_can_node_raise_fault()` で、[node の関数と欄](node.md#異常を知らせるmas_can_node_raise_fault) にあります。点検の項目は [守ること](rules.md#異常) にあります。
- **トランシーバの品番と、配線の図。**

## このページについて

2026-11-15 に書き、同じ日に「雛形と違う基板」を足しました。中身は、雛形（`examples/node_md_rp2040`）のコードと、ライブラリの基板側（`lib/mas/include/mas/can/node.h`、`lib/mas/src/can/node.c`）と突き合わせました。「雛形と違う基板」で引いたコードは、`firmware/CAN-Servo-v1/src/main.cpp` と `firmware/CAN_MD/App/app.c` にあるものです。雛形のビルドは確かめましたが、モーター 2 個の形と DIP スイッチの読み方は、ビルドでも実機でも確かめていません。
