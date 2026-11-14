# やりたいこと別

> **下書き（2026-11-15）。** 書いてある関数の名前、引数、数字は、ライブラリのコード（`lib/mas/include/mas/can/` のヘッダと `lib/mas/src/can/`）と例（`examples/`）に突き合わせました。実機では確かめていません。

| | |
|---|---|
| 読む人 | 主制御のプログラムを書く人 |
| 前提 | [はじめて回す](first-run.md) を済ませ、モーターを 1 個回した |
| 読むとできること | 速さや電流を指定して回す、止める、緊急停止をかける、異常を拾う、サーボを角度で動かす、コントローラを読む、モーターを増やす、線を 2 本使う |

このページのコードは、どれも [はじめて回す](first-run.md) のプログラムに足す形で書いています。`namespace mcan = mas::can;` と `mcan::Host can;` があり、`loop()` の中で `can.update()` を毎回呼んでいる前提です。`set` で始まる関数は値を置くだけで、基板に届けるのは `can.update()` です。

## 速さを指定して回す

`setSpeed` に、回る速さを rad/s で渡します。rad/s は、1 秒に回る角度をラジアンで表したものです。1 回転は約 6.28 rad なので、`setSpeed(6.28f)` は 1 秒に約 1 回転です。

**速さの指令をこなせる基板は、今は XDrive の中継機だけです。** CAN_MD と micro-MD は速さを測れないので、`setSpeed` を受けると止まります。基板ごとにこなせる指令は、[今ある基板](../boards.md) の一覧にあります。

次は [`examples/api_tour`](../../../examples/api_tour/src/main.cpp) の、車輪 4 個を速さで回す部分です。`pad` はコントローラの取っ手で、下の「コントローラ（ゲームパッド）を読む」で説明します。

```cpp
mcan::Md wheel[4] = {can.md(0), can.md(1), can.md(2), can.md(3)};

void setup() {
    // ...
    for (auto &w : wheel) w.configSpeedResolution(0.01f);  // rad/s per LSB
}

void loop() {
    can.update();
    // ...
    float drive = pad.leftY() * 20.0f;
    float turn = pad.rightX() * 10.0f;
    wheel[0].setSpeed(drive + turn);
    wheel[1].setSpeed(drive - turn);
    wheel[2].setSpeed(drive + turn);
    wheel[3].setSpeed(drive - turn);
}
```

気をつけること:

- **送れる速さには上限があります。** 速さは「刻み × 整数」の形で基板に届くので、上限は刻みの 32767 倍です。何も指定しないときの刻みは約 0.105 rad/s で、上限は約 3431 rad/s です。上の例のように `configSpeedResolution(0.01f)` で刻みを 0.01 rad/s にすると、上限は約 327.7 rad/s になります。
- **上限を超えた値は、上限に丸めて送ります。** 今の刻みでの上限は、`wheel[0].speedRange()` で分かります。刻みのしくみは、[しくみ](how-it-works.md) の「「つながっている」と「準備ができた」」にあります。
- **同じ組の 4 個は、どれも速さで動かします。** 0〜3 番は 1 つの組です。組の中で指令の種類を混ぜると、一部のモーターが回りません。下の「モーターを増やすときの番号の決め方」を見てください。

## 力（電流）を指定して回す

`setCurrent` に、モーターに流す電流を A（アンペア）で渡します。モーターが回す力は電流にほぼ比例するので、電流を決めると力を決めたことになります。押し付ける、持ち上げる、のように力を決めたいときに使います。

電流の指令をこなせる基板は、CAN_MD、micro-MD、XDrive の中継機です。CAN-Servo のモーターは電流を測れないので、使えません。

次は [`examples/api_tour`](../../../examples/api_tour/src/main.cpp) の、昇降を電流で動かす部分です。リミットスイッチの行は、汎用ピンがまだ使えないので除いています（下の「汎用ピン、エアー、ソレノイドは、まだ使えません」）。

```cpp
mcan::Md lift = can.md(4);

void setup() {
    // ...
    lift.configCurrentResolution(0.002f); // A per LSB
}

void loop() {
    can.update();
    // ...
    float up = (pad.r2() - pad.l2()) * 3.0f;
    lift.setCurrent(up);
}
```

気をつけること:

- **送れる電流には、ライブラリの上限があります。** 何も指定しないときの刻みは 0.001 A で、上限は約 32.8 A です。上の例のように刻みを 0.002 A にすると、上限は約 65.5 A です。今の上限は `lift.currentRange()` で分かります。上限を超えた値は、上限に丸めて送ります。
- **基板にも上限があります。** CAN_MD は 15 A、micro-MD は 10 A です。決まりでは、基板は自分の上限で電流を抑え、抑えている間は `lift.feedback().currentLimited()` が `true` になります。
- **小さな電流は、思ったとおりに出ないことがあります。** CAN_MD と micro-MD は、電流の指令では duty が一定より下がりません。CAN_MD は 0.345、micro-MD は 0.23 です。回っているモーターは、その強さで回り続けます。詳しくは [今ある基板](../boards.md) にあります。
- **同じ組の 4 個は、どれも電流で動かします。** 理由は速さのときと同じです。

## 止める

止め方は 3 つあります。

| 書き方 | 何が起きるか | 使うとき |
|---|---|---|
| `motor.stop()` | 今の指令の種類のまま、0 を送ります（速さ 0、電流 0、duty 0） | ふだんはこれ |
| `motor.setDuty(0.0f)` | duty 0 で、ブレーキをかけます | duty で回しているモーター |
| `motor.setDuty(0.0f, false)` | duty 0 で、空転させます | duty で回しているモーター |

ブレーキをかけると、モーターはすばやく止まります。空転では、モーターは惰性で回り続けてから止まります。`setDuty` の 2 つ目の引数がこの選び方で、書かなければ `true`（ブレーキ）です。

次は [`examples/api_tour`](../../../examples/api_tour/src/main.cpp) の、コントローラとの無線が切れたときに全部を止める部分です。

```cpp
if (!pad.connected()) {
    for (auto &w : wheel) w.stop();
    lift.stop();
    roller.stop();
    return;
}
```

気をつけること:

- **速さや電流で回しているモーターは、`stop()` で止めます。** `setDuty(0.0f)` を使うと、指令の種類が duty に変わります。同じ組のほかのモーターと種類がぶつかり、どちらかが回らなくなります。XDrive の中継機は、そもそも duty をこなせません。
- **`stop()` は、ブレーキか空転かを変えません。** 直前に `setDuty(0.5f, false)` を置いていたなら、`stop()` の後も空転です。
- **まだ一度も指令を置いていない取っ手では、`stop()` は何もしません。** この場合、基板には指令が届いていないので、基板はもともと止まっています。
- **基板によって、出せる止め方が違います。** CAN_MD と micro-MD はいつもブレーキで、空転はできません。CAN-Servo のモーターはいつも空転で、ブレーキはできません。詳しくは [今ある基板](../boards.md) にあります。
- **線の上の全部を止めるときは、緊急停止を使います。** 下の「全部を緊急停止する、解除する」にあります。

## 回転した量を読む

`position()` が、モーターが回った量を返します。単位は「カウント」という整数で、1 回転が 65536 カウントです。この数は `mcan::kCountsPerRev` という名前でも使えます。ラジアンに直すには `mcan::countsToRad()` を使います。

[`examples/api_tour`](../../../examples/api_tour/src/main.cpp) は、車輪の回った量をラジアンで表示しています。

```cpp
mcan::countsToRad(w.position())
```

ライブラリには、原点を決める関数がありません。原点からの量を知りたいときは、原点に来たときの値を覚えておき、差を取ります。

```cpp
int64_t origin = 0;

// 原点に来たとき
origin = wheel[0].position();

// 原点からの角度（rad）
float rad = mcan::countsToRad(wheel[0].position() - origin);
```

気をつけること:

- **差は、ラジアンに直す前に取ります。** `position()` は `int64_t` で、`countsToRad()` は `float` を返します。大きな数を `float` に直すと細かい桁が落ちるので、先に整数のまま引きます。
- **位置を返す基板は、今は XDrive の中継機だけです。** CAN_MD、micro-MD、CAN-Servo のモーターでは、`position()` は 0 のままです。
- **位置は、基板の電源が入ったときを 0 として数えます。** 基板が途中で再起動すると、止まっていた間に動いた分は数えられません。原点を使っているなら、合わせ直してください。詳しくは [しくみ](how-it-works.md) の「基板が途中で再起動したとき」にあります。

## 速さと電流を読む

`speed()` が速さを rad/s で、`current()` が電流を A で返します。基板が返す値をまとめて読むときは、`feedback()` を使います。

| `feedback()` の中身 | 意味 |
|---|---|
| `current` | 電流（A） |
| `speed` | 速さ（rad/s） |
| `position` | 回った量（カウント） |
| `duty` | 基板が実際に出している duty（−1〜+1） |
| `online` | 基板の声が 0.2 秒以内に届いている |
| `currentValid()`、`speedValid()` | `current`、`speed` が測った値である |
| `currentLimited()` | 基板が電流を上限で抑えている |
| `stopped()` | 基板がモーターを止めている |
| `estopLatched()` | 緊急停止で止まっている |
| `hasFault()` | 基板が異常を知らせている |

[`examples/md_serial_test`](../../../examples/md_serial_test/src/main.cpp) は、10 ミリ秒ごとに、時刻、電流、duty を表示します。

```cpp
const auto &fb = md.feedback();
Serial.printf("%lu,%.3f,%.3f\n", (unsigned long)last, fb.current, fb.duty);
```

気をつけること:

- **測れない値は 0 です。** 0 が「止まっている」のか「測れない」のかは、`currentValid()` と `speedValid()` で見分けます。速さを返す基板は、今は XDrive の中継機だけです。電流は、CAN-Servo のモーター以外が返します。
- **準備ができるまでも 0 です。** ライブラリは、基板との間で刻みを合わせ終えるまで、電流と速さを 0 にします。下の「準備ができるまで待つ」にあります。
- **`duty` は、電流と速さの両方を返す基板では 0 です。** XDrive の中継機がこれに当たります。
- **値が変わるのは `can.update()` の中だけです。** `update()` を呼ばずに何度読んでも、同じ値です。

## 全部を緊急停止する、解除する

`can.estop()` を呼ぶと、線につながっている全部の基板が止まり、止まったままになります。`can.clearEstop()` で解除します。緊急停止の間は、`can.estopped()` が `true` です。

次は [`examples/api_tour`](../../../examples/api_tour/src/main.cpp) の、コントローラの L1 と R1 を同時に押すと緊急停止し、上を押すと解除する部分です。

```cpp
if (pad.held(mcan::Button::L1) && pad.held(mcan::Button::R1)) can.estop();
if (can.estopped() && pad.pressed(mcan::Button::Up)) can.clearEstop();
```

気をつけること:

- **解除の前に、誰が、なぜ緊急停止をかけたのかを確かめてください。** 機体のそばに人がいないかも見ます。
- **解除のとき、ライブラリは、置いてあった値を全部「止まれ」に戻します。** 解除した瞬間に、前の値で動き出さないようにするためです。サーボも力が抜けた状態に戻ります。動かすには、解除の後でもう一度値を置きます。`loop()` で毎回値を置いているプログラムは、解除した次の周から動き出します。理由は [しくみ](how-it-works.md) の「緊急停止」にあります。
- **ほかの主制御がかけた緊急停止でも、`can.estopped()` は `true` になります。** 同じ線の上の誰かが解除すると、`false` に戻ります。
- **緊急停止は、線 1 本ごとです。** 線を 2 本使っているときは、両方の `Host` で `estop()` を呼びます。下の「CAN の線を 2 本使う」にあります。

## 基板の異常を知る

基板は、過電流や過熱に気づくと、異常を知らせます。プログラムは、問い合わせる方法と、知らせを受ける方法の 2 つで異常を知れます。

| 関数 | 中身 |
|---|---|
| `motor.hasFault()` | 異常があると `true` |
| `motor.faultDetail()` | 異常の中身（bit ごと） |
| `motor.clearFault()` | 異常を消す |
| `can.onFault(関数)` | 新しい異常が見つかったときに、その関数を呼ぶ |

[`examples/md_serial_test`](../../../examples/md_serial_test/src/main.cpp) は、異常の知らせを受けて、シリアルに表示します。

```cpp
void onFault(mcan::Device &dev, uint32_t bits) {
    Serial.printf("! MD %d fault 0x%08lx\n", dev.ch(), (unsigned long)bits);
}

void setup() {
    // ...
    can.onFault(onFault);
}
```

知らせを受ける関数は、`void 名前(mcan::Device &dev, uint32_t bits)` の形で書きます。`dev.ch()` が、異常を知らせた基板の番号です。どのモーターかを比べるときは、`dev == lift` のように書きます（[`examples/api_tour`](../../../examples/api_tour/src/main.cpp) の `onFault`）。

気をつけること:

- **`bits` には、新しく立った bit だけが入ります。** 前から立っている bit では、もう一度は呼ばれません。
- **`faultDetail()` は、少し遅れて埋まります。** `hasFault()` が `true` になった後、ライブラリが基板に中身を問い合わせます。返事が届くまで、`faultDetail()` は 0 です。
- **`clearFault()` は、`faultDetail()` が 0 の間は何もしません。** 中身が分かってから呼んでください。原因が続いていれば、消してもまた立ちます。
- **bit の意味は、仕様書にあります。** [仕様書の「異常の詳細の bit」](../../spec/settings.md#8-異常の詳細の-bit) を見てください。基板の再起動を表す bit だけは、ライブラリが自分で消すので、プログラムには届きません。
- **異常は、消すまで残ります。** 異常のときにモーターを止めるかどうかは、基板によります。

## 準備ができるまで待つ

`can.waitReady(ミリ秒)` は、基板との準備が全部済むまで待ちます。済めば `true`、決めた時間を過ぎれば `false` を返します。[`examples/md_minimal`](../../../examples/md_minimal/src/main.cpp) は、`setup()` の中で最大 1000 ミリ秒待ちます。

```cpp
if (!can.begin(CAN_TX, CAN_RX)) Serial.println(mcan::toString(can.error()));
if (!can.waitReady(1000)) Serial.println("MD not ready");
```

`waitReady` が待つ相手は、プログラムが取っ手を作った相手です。

| 取っ手 | 待つ条件 |
|---|---|
| モーター（`can.md`） | `online()` と `ready()` が両方 `true` |
| コントローラ（`can.controller`） | `online()` が `true` |
| サーボ（`can.servo`） | 待ちません（返事をしないため） |

気をつけること:

- **`can.begin()` の後で呼びます。** `begin()` の前に呼ぶと、すぐに `false` を返します。
- **つないでいない相手の取っ手があると、必ず `false` になります。** 時間いっぱい待ってから戻ります。
- **`false` のときは、取っ手ごとに `online()` と `ready()` を表示して、足りない相手を探します。** 見方は [動かないとき](troubleshooting.md) にあります。
- **待たなくても、準備が済めば動き出します。** 準備が済むまで、ライブラリは「止まれ」を送ります。`waitReady` は、準備が済んだかをプログラムが知るためのものです。
- **`loop()` の中では呼ばないでください。** 待っている間、`loop()` のほかの処理が止まるからです。

## サーボを角度で動かす、力を抜く

`setAngle` に、角度をラジアンで渡します。角度とサーボの信号（パルス幅）の対応は、`configCalibration` で教えます。サーボは機種によって、同じ信号でも向く角度が違うからです。

[`examples/servo_minimal`](../../../examples/servo_minimal/src/main.cpp) は、−90° で 500 µs、+90° で 2500 µs と教えてから、±1 rad をゆっくり往復させます。

```cpp
mcan::Servo arm = can.servo(0); // ch 0 のサーボ

void setup() {
    // ...
    arm.configCalibration(-1.57f, 1.57f, 500, 2500); // -90° で 500 µs、+90° で 2500 µs
}

void loop() {
    can.update();

    float t = millis() * 0.001f;
    arm.setAngle(sinf(t) * 1.0f); // ±1 rad をゆっくり往復
}
```

ほかの書き方:

- **パルス幅で直接動かす。** `arm.setPulseUs(1500);` のように、µs で渡します。ライブラリは 10 µs 刻みに丸め、500〜2500 µs に抑えます。
- **力を抜く。** `arm.release();` で、サーボへの信号を止めます。もう一度 `setAngle` か `setPulseUs` を呼ぶと、また動きます。
- **向きを逆にする。** `configCalibration` の 2 つのパルス幅を入れ替えます。`arm.configCalibration(-1.57f, 1.57f, 2500, 500);` です。

気をつけること:

- **何も指定しないときの対応は、−1.5708 rad で 500 µs、+1.5708 rad で 2500 µs です。** `configCalibration` の値は主制御だけが覚え、角度をパルス幅に直してから送ります。
- **範囲の外の角度は、500 µs か 2500 µs で止まります。** 2 点の外側は直線で伸ばしてから、500〜2500 µs に抑えるためです。
- **計算が壊れた値（NaN）は無視します。** そのときは、前の指令をそのまま保ちます。
- **取っ手を作った直後のサーボは、力が抜けています。** 最初の `setAngle` か `setPulseUs` で動き出します。緊急停止を解除した後も、同じく力が抜けた状態です。
- **サーボの基板は返事をしません。** 届いているかは、基板の LED で見ます。色の意味は [はじめて回す](first-run.md) の「サーボを動かす」にあります。

## コントローラ（ゲームパッド）を読む

`can.controller(番号)` で、コントローラの取っ手を受け取ります。番号は 0〜7 です。コントローラは、受信機の基板を通して線につながります。

> **このリポジトリには、コントローラの受信機の基板がありません。** そのため、この節のコードは、本物の受信機で確かめていません。ライブラリの関数だけを書いています。

| 関数 | 返すもの |
|---|---|
| `pad.online()` | 受信機の基板の声が 0.2 秒以内に届いていると `true` |
| `pad.connected()` | 上に加えて、パッドとの無線がつながっていると `true` |
| `pad.leftX()`、`pad.leftY()`、`pad.rightX()`、`pad.rightY()` | スティック。−1〜+1。右と上が正 |
| `pad.l2()`、`pad.r2()` | トリガー。0〜1 |
| `pad.held(ボタン)` | 押している間 `true` |
| `pad.pressed(ボタン)` | 押した瞬間だけ `true` |
| `pad.released(ボタン)` | 離した瞬間だけ `true` |
| `pad.configDeadzone(半径)` | スティックの遊び。0 以上 1 未満。何も指定しなければ 0（遊び無し） |

ボタンは、位置で名前が付いています。

| 名前 | ボタン |
|---|---|
| `Up`、`Down`、`Left`、`Right` | 十字キー |
| `North`、`South`、`West`、`East` | 右の 4 つ。North が △（Y）、South が ×（A）、West が □（X）、East が ○（B） |
| `L1`、`R1` | 肩のボタン |
| `L3`、`R3` | スティックの押し込み |
| `Select` | 中央の左（Share、View、−） |
| `Start` | 中央の右（Options、Menu、＋） |

ボタンの名前は、`mcan::Button::North` のように書きます。L2 と R2 は、ボタンではなくトリガーとして `l2()`、`r2()` で読みます。

次は [`examples/api_tour`](../../../examples/api_tour/src/main.cpp) から、コントローラを使う行を抜き出して、短くしたものです。

```cpp
mcan::Controller pad = can.controller(0);

void setup() {
    // ...
    pad.configDeadzone(0.05f);
}

void loop() {
    can.update();
    // ...
    if (!pad.connected()) {
        // ここでモーターを止めて、return する（上の「止める」）
    }
    float drive = pad.leftY() * 20.0f;
    if (pad.held(mcan::Button::North)) {
        roller.setDuty(0.6f);
    } else if (pad.released(mcan::Button::North)) {
        roller.setDuty(0.0f, true);
    }
}
```

気をつけること:

- **無線が切れたら、モーターを止めてください。** `pad.connected()` が `false` のときです。止めないと、最後に置いた値でモーターが回り続けます。
- **受信機の基板の声が 0.2 秒届かないと、ライブラリがスティックを 0 に、ボタンを離した状態にします。** そのとき、`online()` と `connected()` は `false` です。
- **`pressed` と `released` は、`can.update()` の後で読みます。** 「押した瞬間」は、次の `update()` を呼ぶと消えるからです。`loop()` の 1 周で 1 回だけ `true` になる、と考えてください。
- **遊びは、スティックごとに円の形でかかります。** 半径の内側は 0 で、外側は 0 から 1 まで伸ばし直します。トリガーにはかかりません。

## モーターを増やすときの番号の決め方

**モーターの番号は、4 個ずつの組で、指令の種類をそろえて決めます。** 0〜3 番、4〜7 番、8〜11 番、のように 4 個ずつが 1 組です。1 組の中で `setDuty`、`setCurrent`、`setSpeed` を混ぜると、番号のいちばん小さいモーターの種類だけが送られます。同じ種類のモーターは動きますが、違う種類のモーターは回りません。

[`examples/api_tour`](../../../examples/api_tour/src/main.cpp) は、種類ごとに組を分けています（コメントは書き換えています）。

```cpp
mcan::Md wheel[4] = {can.md(0), can.md(1), can.md(2), can.md(3)}; // 速さ。0〜3 番の組
mcan::Md lift = can.md(4);   // 電流。4〜7 番の組
mcan::Md roller = can.md(8); // duty。8〜11 番の組
```

種類が混ざっているかは、プログラムから分かります。回らないほうのモーターの `commandConflict()` が `true` になり、`can.error()` が `mcan::Error::CommandConflict` を返します。しくみは [しくみ](how-it-works.md) の「4 個ずつ、まとめて送っている」にあります。

気をつけること:

- **モーターの番号は 0〜31 です。** `can.md(32)` のように範囲の外を求めると、何もしない取っ手が返ります。その取っ手の `ch()` は 255 です。
- **4 個ぶんの番号を持つ基板は、1 つの組をまるごと使います。** XDrive の中継機と、CAN-Servo のモーターがそうです。先頭の番号は 4 の倍数です。
- **基板ごとに、使える番号が違います。** CAN_MD は DIP スイッチで 0〜7、micro-MD は 0〜31 です。CAN-Servo と XDrive の中継機は、ファームを書き込むときに決めます。詳しくは [今ある基板](../boards.md) にあります。
- **同じ番号のモーターの基板を 2 枚つながないでください。** どちらも正しく動きません。
- **サーボの番号は、モーターとは別に数えます。** サーボは 0〜63 で、8 個ずつの組です。サーボには指令の種類が 1 つしか無いので、組の心配はありません。

## CAN の線を 2 本使う

**線を 2 本使うときは、`Host` を線ごとに 1 つ作ります。** 番号と緊急停止は、線ごとに別々です。線 0 の「モーターの 0 番」と、線 1 の「モーターの 0 番」は、別の基板です。

**今のライブラリで線を 2 本使えるのは、RP2040（Raspberry Pi Pico）だけです。** RP2040 は、PIO という部品を 2 つ持ち、PIO 0 と PIO 1 に 1 本ずつ線を作れます。ESP32 の CAN の口（`Esp32Twai`）は、ESP32 の CAN の部品を 1 つだけ使う作りなので、線は 1 本です。

リポジトリの [README](../../../README.md) の「最小のプログラム」の節に、CAN の口を自分で作って渡す、RP2040 で線を 1 本の例があります。

```cpp
mcan::Rp2040Can2040 bus;  // RP2040（can2040。PIO 0 と 1 で 2 本まで）

void setup() {
    bus.begin(5, 4, 1);  // TX, RX, PIO
    can.begin(bus);
}
```

これを 2 本にすると、次のようになります。ピンの番号は例です。

```cpp
mcan::Host can0, can1;
mcan::Rp2040Can2040 bus0, bus1;
mcan::Md arm = can0.md(0);   // 線 0 のモーターの 0 番
mcan::Md wheel = can1.md(0); // 線 1 のモーターの 0 番

void setup() {
    Serial.begin(115200);
    if (!bus0.begin(0, 1, 0) || !can0.begin(bus0)) Serial.println("line 0 failed"); // TX, RX, PIO
    if (!bus1.begin(2, 3, 1) || !can1.begin(bus1)) Serial.println("line 1 failed");
}

void loop() {
    can0.update();
    can1.update();
    // ...
}
```

近道の書き方もあります。`can0.begin(0, 1, 0);` のように、`can.begin(TX, RX, PIO)` と書くと、ライブラリが CAN の口を作ります。

気をつけること:

- **`update()` は、両方の `Host` で毎回呼びます。** 片方を忘れると、その線の基板には指令が届かず、止まります。
- **取っ手は、作った `Host` の線の相手を指します。** `can0.md(0)` で作った取っ手は、線 0 の基板だけを動かします。
- **緊急停止も、両方で呼びます。** `can0.estop()` は、線 1 の基板を止めません。機体全体を止めるときは、`can0.estop();` と `can1.estop();` の両方を呼びます。解除も同じです。
- **RP2040 の CAN は、ライブラリを 1 つ足して使います。** `platformio.ini` の `lib_deps` に ACAN2040 を足します。書き方は、リポジトリの [README](../../../README.md) の「入れ方」にあります。

## 値をすぐ送る

`can.flush()` を呼ぶと、値が変わったモーターとサーボの指令を、その場で送ります。ふだんは `can.update()` が、モーターは 10 ミリ秒ごと、サーボは 20 ミリ秒ごとに送ります。そのため、値を置いてから届くまでに、最大でその分だけ遅れます。この遅れを縮めたいときに `flush()` を使います。

```cpp
wheel[0].setSpeed(5.0f);
wheel[1].setSpeed(5.0f);
can.flush(); // 変わった値を、今すぐ送る
```

気をつけること:

- **値を全部置いてから、最後に 1 回呼びます。** 1 個置くたびに呼ぶと、途中までしか変わっていない値が基板に届くからです。
- **`loop()` で毎回呼ぶと、線が混みます。** ライブラリは、送りすぎを止めません。
- **`flush()` が送るのは指令だけです。** 刻みのような設定は、これまでどおり `update()` の中で送ります。
- **`flush()` は `update()` の代わりになりません。** 受け取りや、準備のやりとりは、`update()` だけが行います。

## 汎用ピン、エアー、ソレノイドは、まだ使えません

ライブラリには、`can.pin()`、`can.air()`、`can.solenoid()` という関数があります。**どれも、今は何も送らず、何も受け取りません。** 基板と話すための決まりが、まだできていないからです。値を置いてもどの基板にも届かず、読む関数は 0 か `false` を返します。

[`examples/api_tour`](../../../examples/api_tour/src/main.cpp) はこれらも呼んでいますが、それは関数の形を試すためです。ロボットのプログラムには書かないでください。

## ESP32 の新しい開発環境（pioarduino）

**ESP32 は、ほとんどの場合、PlatformIO 公式の環境で困りません。** [はじめて回す](first-run.md) の `platform = espressif32` がそれで、中身は arduino-esp32 の 2.x という版です。

新しい版の arduino-esp32 3.x を使うには、有志が作っている pioarduino という環境が要ります。今のライブラリは、pioarduino 55.03.312（arduino-esp32 3.3.12）で、何も変えずにビルドできました。確かめたのは `examples/md_minimal` のビルドだけで、実機では動かしていません。

気をつけること:

- **pioarduino を使うと、PC の PlatformIO 本体が pioarduino 版に入れ替わります。** pioarduino は、自分の本体を、PlatformIO の共有の場所（`~/.platformio/penv`）に入れる作りだからです。入れ替わると、その PC のほかのプロジェクトにも影響します。戻すには、`pioarduino` を消して、`platformio` を入れ直します。
- **このリポジトリの例には、pioarduino の環境を入れていません。** 例のフォルダで `pio run` をすると全部の環境がビルドされ、知らないうちに本体が入れ替わってしまうからです。
- **新しい版では、ライブラリが使っている CAN のドライバ（`driver/twai.h`）が古い書き方の扱いになっています。** 今は動きます。

## このページにまだ書いていないこと

確かめられなかったので、書いていません。

- コントローラの受信機の基板。このリポジトリに無いので、受信機の番号の決め方、つなぎ方、実機でのスティックとボタンの動きを確かめていません。
- RP2040 で、2 本目の線にどのピンを使えるか。
- XDrive の中継機で、`position()` の 1 回転（65536 カウント）が、何の 1 回転に当たるか（モーターの軸か、減速した後か）。
- 電流の指令で、基板が上限に当たったときに `currentLimited()` が実際に `true` になるか（基板ごとのファームで確かめていません）。
- 線の混み具合（`can.busLoad()`）と、受け取りの取りこぼし（`can.rxOverflows()`）の使い方。
- このページの全部を、実機では確かめていません。
