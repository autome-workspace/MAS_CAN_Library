# はじめて回す

| | |
|---|---|
| 読む人 | 主制御のプログラムを書く人（MAS CAN がはじめての人） |
| 前提 | PlatformIO で、LED を点滅させるプログラムを書いて、マイコンに書き込める |
| 読むとできること | モーターを 1 個回す |

CAN という通信の中身は、このページでは知らなくて大丈夫です。知っておくのは次の 3 つだけです。

- **主制御と基板を、2 本の線（H と L）でつなぎます。** 基板が何枚あっても、同じ 2 本につなぎ足します。
- **線の両端に、終端抵抗が要ります。** 無いと通信が不安定になります。
- **基板には番号があります。** プログラムは「モータードライバーの 0 番」のように、番号で相手を選びます。

## 用意するもの

| もの | 補足 |
|---|---|
| 主制御にするマイコン | ESP32、ESP32-S3、Raspberry Pi Pico のどれか |
| CAN のトランシーバ | マイコンの信号を、CAN の線の信号に変える部品です。部でよく使うのは ATA6561 です |
| モータードライバーの基板 | CAN_MD か micro-MD。モーターをつないでおきます |
| 電源 | 24 V |
| PC と USB ケーブル | PlatformIO が入っているもの |

モーターは、このページの最後で実際に回ります。回っても危なくないように、固定するか、何も付けていない軸にしておいてください。

## 1. つなぐ

配線の詳しいことは [つなぎ方](../wiring.md) にあります。まだ書けていない所があり、そこには「先輩に確かめてください」と書いてあります。**書いていないことは、推測でつながないでください。** 電源を逆につなぐと、基板が壊れることがあります。

1. **マイコンとトランシーバをつなぎます。** このページの例では、ESP32 が TX を GPIO 5、RX を GPIO 4 に、Pico が TX を GPIO 0、RX を GPIO 1 につなぎます。
2. **トランシーバと基板を、H どうし、L どうしでつなぎます。** CAN_MD は、2 ピンの端子（J27 か J28）の 1 番が H、2 番が L です。
3. **終端抵抗を、線の両端の 2 か所に入れます。** 基板 1 枚と主制御だけなら、基板の側と、主制御（トランシーバ）の側です。
4. **基板に電源（24 V）を入れます。**

H と L を逆につなぐと、どの基板とも話せません。動かないときに、最初に疑う所です。

## 2. 基板の番号と電源を確かめる

モータードライバーの基板は、番号を自分で持っています。この番号を **ch**（チャンネル）と呼びます。プログラムに書く番号と、基板の ch が合っていないと、モーターは回りません。

このページでは **ch を 0** にします。決め方は基板で違います。

| 基板 | ch の決め方 | ch の確かめ方 |
|---|---|---|
| CAN_MD | 基板の DIP スイッチ（SW3）。3 つとも OFF で ch 0 です | LED が ch の回数だけ点滅します。ch 0 のときは消えたままです |
| micro-MD | 基板のスイッチを押して決めます。何も設定していない基板は ch 0 です | 同じです |

LED が点滅していれば、電源が来ていて、ファームも動いています。点滅の回数が 0 でなければ、その数が今の ch です。プログラムの `can.md(0)` の 0 をその数に変えるか、下の手順で基板の ch を 0 にします。

LED が消えたままのときは、ch 0 なのか、電源が来ていないのかを、LED だけでは見分けられません。次のように確かめます。

- **CAN_MD:** DIP スイッチの 1 だけを ON にして、電源を入れ直します。LED が 1 回ずつ点滅すれば、電源は来ています。確かめたらスイッチを OFF に戻し、もう一度電源を入れ直します。基板は、ch を電源を入れたときに 1 回だけ読むからです。
- **micro-MD:** スイッチを 1.5 秒長押しします。LED が点きっぱなしになれば、電源は来ています。そのまま 10 秒何も押さなければ、ch を変えずに元に戻ります。

micro-MD の ch を 0 にするには、スイッチを 1.5 秒長押しし（LED が点きっぱなしになります）、短くは押さずに、もう一度 1.5 秒長押しします。モーターが「ピピッ」と鳴り、基板が再起動すれば保存されています。音はモーターのコイルで出しているので、モーターをつないでいないと鳴りません。

ch の決め方の詳しい手順は [今ある基板](../boards.md) にあります。

## 3. プロジェクトを作る

PlatformIO で新しいプロジェクトを作り、`platformio.ini` を次のようにします。使う主制御の分だけを書きます。

ESP32:

```ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps = https://github.com/autome-workspace/MAS_CAN_Library.git#v1.0.0
build_flags = -D CAN_TX=5 -D CAN_RX=4
```

ESP32-S3 は、`board` の行を `esp32-s3-devkitc-1` に変えます。

Raspberry Pi Pico:

```ini
[env:pico]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git#9c167c6b8aac4f4cfa6d55a0c4e5b848795150c0
board = pico
board_build.core = earlephilhower
framework = arduino
monitor_speed = 115200
lib_deps =
    https://github.com/autome-workspace/MAS_CAN_Library.git#v1.0.0
    https://github.com/obdevel/ACAN2040.git#1.0.6
build_flags = -D CAN_TX=0 -D CAN_RX=1
```

`lib_deps` の行が、取り込むライブラリの指定です。1 行目が MAS CAN のライブラリです。Pico は、CAN を扱うためのライブラリ（ACAN2040）も取り込みます。

`build_flags` の行は、手順 1 でつないだピンの番号です。プログラムの `CAN_TX`、`CAN_RX` に、この番号が入ります。別のピンにつないだなら、ここを変えます。

## 4. プログラムを書く

`src/main.cpp` を次のようにします。[`examples/md_minimal`](../../../examples/md_minimal/src/main.cpp) から、Pico の細かい設定についてのコメントを 1 行除いたものです。

```cpp
#include <Arduino.h>
#include <mas/can.h>

namespace mcan = mas::can;

mcan::Host can;
mcan::Md motor = can.md(0); // ch 0 の MD（基板の DIP で決まる）

void setup() {
    Serial.begin(115200);
    if (!can.begin(CAN_TX, CAN_RX)) Serial.println(mcan::toString(can.error()));
    if (!can.waitReady(1000)) Serial.println("MD not ready");
}

void loop() {
    can.update(); // 送受信はこの中。毎回呼ぶ

    motor.setDuty(0.3f); // 30%。値を置くだけで、送るのは update()

    static uint32_t last = 0;
    if (millis() - last >= 500) {
        last = millis();
        Serial.printf("%s duty %+.2f\n", motor.online() ? "online" : "offline",
                      motor.feedback().duty);
    }
}
```

登場するものは 3 つです。

- **`can`** は、主制御として線の上で話す役です。送ることも受けることも、これがまとめて行います。
- **`motor`** は、「モータードライバーの 0 番」を指す取っ手です。`can.md(0)` の 0 が、手順 2 で確かめた ch です。
- **`can.update()`** が、実際に送り、受けます。`loop()` の中で毎回呼びます。`motor.setDuty(0.3f)` は値を置くだけで、その値を基板に届けるのは `update()` です。

`update()` は、1 秒に 100 回以上呼ばれるようにしてください。`loop()` の中に `delay(100)` のような長い待ちを入れると、基板は「主制御が止まった」と見て、安全のためにモーターを止めます。

## 5. 書き込んで、回るのを見る

1. プログラムを書き込みます。
2. シリアルモニタを開きます（115200）。
3. 次のように出て、モーターが回れば成功です。

```
online duty +0.30
online duty +0.30
```

`online` は、基板の声が主制御に届いている、という意味です。`duty` は、基板が実際に出している強さです。

`setDuty` の値は −1.0 から +1.0 です。`0.3f` を `-0.3f` にすると、逆に回ります。`0.0f` にすると止まります。

`online` と出ているのにモーターが回らないときは、値が小さすぎるのかもしれません。モーターは、ある強さを超えるまで動き出しません。`0.5f` に上げて試してください。

**止めるには**、`0.3f` を `0.0f` に変えて書き込み直します。急ぐときは、主制御の USB を抜きます。指令が途切れるので、基板は 0.1 秒でモーターを止めます。

次のように出たら、基板と話せていません。[動かないとき](troubleshooting.md) に進んでください。何も表示されない、主制御が再起動を繰り返す、ほかの文が出る、というときも同じです。

```
MD not ready
offline duty +0.00
```

## 次に読むもの

- 動かなかった: [動かないとき](troubleshooting.md)
- サーボを動かしたい: [はじめてサーボを動かす](first-servo.md)
- 速さを指定して回したい、緊急停止を使いたい: [やりたいこと別](howto.md)
- 中で何が起きているかを知りたい: [しくみ](how-it-works.md)

## このページについて

2026-11-15 に書き、同じ日に直しました。プログラムと表示は例（`examples/md_minimal`）と、`platformio.ini` は同じ例の `platformio.ini` と、ch と LED は [今ある基板](../boards.md) と [`firmware/micro-MD/README.md`](../../../firmware/micro-MD/README.md) と突き合わせました。Pico の `platformio.ini` は、例の設定をリポジトリの外のプロジェクト用に書き直したもので、この形でのビルドは確かめていません。配線の、まだ書けていない所は [つなぎ方](../wiring.md) の最後にまとめてあります。
