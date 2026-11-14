# マイコンごとの組み込み

| | |
|---|---|
| 読む人 | 基板のファームを作る部員で、雛形と違うマイコンか、雛形と違う作りで書く人 |
| 前提 | [雛形から始める](start.md) を読んだ。作るマイコンで、CAN の送受信とフラッシュの書き込みを、データシートやマイコンの SDK を見て書ける |
| 読むとできること | 自分のマイコンのファームに node を足してビルドでき、CAN の送受信、時刻、フラッシュ、固有 ID、再起動を、手本のファームの関数を見ながら書ける |

**マイコンごとの共通の部品は、まだありません。** 今は、3 つのファームが、それぞれ自分で書いています。このページは、今ある 3 通りのやり方と、それぞれでどの関数が手本になるかを並べます。

| やり方 | マイコン | 手本のファーム |
|---|---|---|
| PlatformIO と Arduino | RP2040 | 雛形 [`examples/node_md_rp2040`](../../../examples/node_md_rp2040/)、[`firmware/CAN-Servo-v1`](../../../firmware/CAN-Servo-v1/) |
| PlatformIO で、Arduino でない | CH32V203 | [`firmware/micro-MD`](../../../firmware/micro-MD/) |
| CMake | STM32F042 | [`firmware/CAN_MD`](../../../firmware/CAN_MD/) |

## node をビルドに足す

node に要るのは、次の 3 つの C のファイルと、インクルードの道です。C11 でコンパイルします。

| ファイル | 中身 |
|---|---|
| `lib/mas/src/can/node.c` | node |
| `lib/mas/src/can/core/md_frame.c` | MD の指令と FB の形 |
| `lib/mas/src/can/core/setting.c` | 設定と、緊急停止などの共通のフレームの形 |
| `lib/mas/include` | インクルードの道。ファームは `#include "mas/can/node.h"` と書きます |

`lib/mas` には、ライブラリとして取り込むための `lib/mas/library.json` があります。ただし、これは Arduino（ESP32、RP2040）向けで、主制御のライブラリ（C++）も含みます。Arduino でない基板では、ライブラリごと取り込まず、上の 3 つだけを足します。

### PlatformIO と Arduino（RP2040）

雛形と CAN-Servo のファームの `platformio.ini` は、`lib_extra_dirs` で `lib` のフォルダを見せ、CAN のライブラリ（ACAN2040）を `lib_deps` で取り込んでいます。雛形の `platformio.ini` のこの部分です。

```ini
lib_extra_dirs = ../../lib
lib_deps =
    https://github.com/obdevel/ACAN2040.git#1.0.6
```

`../../lib` は、`platformio.ini` のあるフォルダから 2 段上の `lib` です。`examples/` の下と `firmware/` の下は、どちらも 2 段なので、この行のまま使えます。ACAN2040 は、RP2040 の PIO で CAN を動かす can2040 を、Arduino から使えるようにしたライブラリです。

### PlatformIO で、Arduino でない（CH32V203）

micro-MD は、WCH の SDK（`framework = noneos-sdk`）で書いています。`platformio.ini` でインクルードの道を足し、`mas_node.py` という小さなスクリプトで、node の 3 つのファイルだけをビルドに足しています。`platformio.ini` のこの部分です。

```ini
build_flags =
    -D HSE_VALUE=16000000
    -I ../../lib/mas/include
    -Wall
extra_scripts = mas_node.py
```

`mas_node.py` の全部です。

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

`src_filter` が、`lib/mas/src/can` の中から 3 つのファイルだけを選んでいます。道は、リポジトリの中の置き場所からの相対です。ファームを別の深さに置くなら、`../../lib/mas` を直します。

### CMake（STM32F042）

CAN_MD は、CubeMX が作った CMake のプロジェクトです。`CMakeLists.txt` の、利用者が書き足してよい所に、node の 3 つのファイルとインクルードの道を足しています。

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    App/app.c
    ../../lib/mas/src/can/core/md_frame.c
    ../../lib/mas/src/can/core/setting.c
    ../../lib/mas/src/can/node.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    App
    ../../lib/mas/include
)
```

CubeMX が作ったファイルは触らず、ハードウェアに触る部分は `App/app.c` に置いています。`Core/Src/main.c` の `USER CODE` の枠から、`app_init()` と `app_loop()` を呼ぶ形です。CubeMX で作り直しても、この枠の中は残るからです。ビルドは Release で行います（`cmake --preset Release && cmake --build --preset Release`）。Debug でビルドすると、FLASH に入りきりません。

## ファームが自分で用意するもの

どのマイコンでも、ファームは次のものを用意します。表の中の名前は、手本のファームの関数です。

| 用意するもの | RP2040（雛形） | CH32V203（micro-MD） | STM32F042（CAN_MD） |
|---|---|---|---|
| CAN の初期化（速さ、受信フィルタ） | `ACAN2040 can(...)` と `can.begin()`、`reserveCanPio()` | `can_init()`、`can_filter()` | CubeMX が作る `MX_CAN_Init()`（`Core/Src/main.c`）と、`app_init()` の中の `can_filter()`、`HAL_CAN_Start()` |
| 受信（割り込みで輪に積む） | `onCan()` | `USB_LP_CAN1_RX0_IRQHandler()` | `HAL_CAN_RxFifo0MsgPendingCallback()` |
| 送信 | `canSend()` | `can_send()` | `can_send()` |
| 時刻（µs） | `micros()` | `now_us()`（TIM4） | `now_us()`（`HAL_GetTick()` に 1000 を掛ける） |
| フラッシュ（`load`、`save`） | `settingsLoad()`、`settingsSave()`（Arduino の `EEPROM`） | `flash_load()`、`flash_save()`、`flash_write_page()` | `flash_load()`、`flash_save()` |
| 固有 ID | `uniqueId()` | `unique_id()` | `unique_id()` |
| 再起動 | `restart()` | `restart()` | `restart()`（待たない。写さない） |
| バスオフからの復帰の知らせ | 無し（can2040 に仕組みが無い） | `watch_bus_off()` | 無し（見張っていない。写さない） |
| 基板の番号 | ビルドで渡す（`-D MD_CH=0`） | `load_k()`、`save_k()`、`ui_update()`（LED とスイッチで決めてフラッシュに覚える） | `read_dip()`（DIP スイッチ 3 本）に、ビルドで渡す `CAN_MD_FIRST_CH` を足す |

受信は、どのファームも同じ形です。割り込みで、標準 ID のフレームだけを `mas_can_frame_t` の輪に積み、`rtr` を埋めます。輪が満ちていれば捨てます。メインのループが輪から取り出して、`mas_can_node_on_frame` に渡します。

### RP2040

- **CAN は can2040 で、PIO の 1 番を使います。** can2040 は PIO の 4 つのステートマシンを予約せずに使うので、`reserveCanPio()` で先に予約しています。
- **can2040 には、ハードウェアの受信フィルタがありません。** 受けたフレームを全部 node に渡して構いません。node は関係の無い ID を捨てます。
- **サンプル点は 81.25% で固定です。** 決まりは 80% ですが、can2040 だけはこれを認めています。
- **フラッシュは、Arduino の `EEPROM` でフラッシュの末尾に置きます。** 書いている間は割り込みもメインのループも止まるので、`settingsSave()` は書く前に出力を切っています。
- **固有 ID は、フラッシュの 64 bit の番号を、先頭の byte を上位にして並べます。** `uniqueId()` です。
- **再起動は、送り終えの知らせを数えて待ちます。** `canSend()` が渡した本数を `txInFlight` で数え、`onCan()` が送り終えの知らせで減らします。`restart()` は、これが 0 になるのを最大 10 ms 待ってから `rp2040.reboot()` を呼びます。

### CH32V203

- **時計は、16 MHz のセラロックから 120 MHz を作ります（`clock_init()`）。** セラロックが起動しなければ内蔵の発振器で同じ周波数にしますが、そのときは CAN に要る精度が足りません。
- **CAN は 1 Mbps、サンプル点 80% です。** `can_init()` は、60 MHz を 3 で割った 20 TQ（1 ＋ 15 ＋ 4）、SJW 4 TQ にし、バスオフからの自動の復帰（`CAN_ABOM`）を有効にしています。受信フィルタは 4 つ（`can_filter()`）です。
- **時刻は、1 MHz で回る 16 bit のタイマー（TIM4）の一周を、`now_us()` が呼ばれたときに数えて 32 bit にします。** タイマーは 65.5 ms で一周するので、それより長く `now_us()` を呼ばない間があると、時刻がずれます。
- **フラッシュは、最後の 1 KB を使います。** `link.ld` でプログラムから外してあります。`flash_write_page()` は 256 byte のページを消して書き、その前にモーターをブレーキにします。
- **固有 ID は、マイコンの 96 bit の番号から作ります（`unique_id()`）。**
- **再起動は、送信の箱が 3 つとも空くのを、最大 10 ms 待ちます（`restart()`）。**
- **バスオフは、`watch_bus_off()` が毎周見張ります。** 立っていた印が消えたら、node に知らせます。

### STM32F042

- **CAN の速さと発振は、CubeMX の設定（`CAN_MD.ioc`）で決まり、`Core/Src/main.c` の `MX_CAN_Init()` と `SystemClock_Config()` に書き出されます。** 今の値は、サンプル点が 75% で、発振は内蔵の発振器（HSI）です。どちらも決まりと違うので、写さないでください。
- **受信フィルタは、`app_init()` の中で `can_filter()` を 4 回呼んで設定します。** その後、`HAL_CAN_Start()` で CAN を始めます。
- **時刻は、1 ms 刻みの `HAL_GetTick()` に 1000 を掛けた値です。** 粗くても node は動きますが、FB の周期は 1 ms 刻みに丸まります。
- **フラッシュは、最後のページ（1 KB、`0x08007C00`）を使います。** リンカスクリプト（`STM32F042XX_FLASH.ld`）でプログラムから外してあります。`flash_save()` は、書く前にモーターをブレーキにします。
- **固有 ID は、マイコンの 96 bit の番号から作ります（`unique_id()`）。**
- **再起動とバスオフは、写さないでください。** `restart()` は返事が線に出るのを待たずに再起動し、バスオフは自動の復帰が有効なだけで、復帰を node に知らせていません。

## 受信フィルタ

基板が受ける ID と送る ID は、仕様書の [受信フィルタ](../../spec/board.md#受信フィルタ) にあります。micro-MD のフィルタを例に、マスクの読み方も書いてあります。

決まりより広く通しても構いません。node は、関係の無い ID のフレームを捨てるからです。ただし、リモートフレームの `0x000`（緊急停止）は必ず通します。また、通したリモートフレームを node が見分けられるよう、受信で `rtr` を必ず埋めます。

## 既存のファーム

| 場所 | マイコンと作り | 番号の決め方 | 受け持つもの |
|---|---|---|---|
| `examples/node_md_rp2040` | RP2040、Arduino、can2040、PlatformIO | ビルドで渡す（`-D MD_CH=0`） | 新しい基板の雛形。MD 1 個、duty だけ。基板の種類は `0x0000` のまま |
| `firmware/CAN-Servo-v1` | RP2040-Zero、Arduino、can2040、PlatformIO | ビルドで渡す（`SERVO_FIRST_CH`、`MD_FIRST_CH`） | サーボ 8 個と MD 4 個（duty だけ）。MD は node、サーボはファームが自分で受け持つ |
| `firmware/micro-MD` | CH32V203K8T6、WCH の SDK（noneos-sdk）、PlatformIO | LED とスイッチで決めて、フラッシュに覚える | MD 1 個、duty と電流 |
| `firmware/CAN_MD` | STM32F042、HAL（CubeMX）、CMake。Release でビルドする | DIP スイッチ 3 本に、ビルドで渡す値（`-D CAN_MD_FIRST_CH=n`）を足す | MD 1 個、duty と電流 |

既存のファームには、決まりと違う所があります。新しい基板に写すときは、仕様書の [既存のファームが決まりと違う所](../../spec/board.md#既存のファームが決まりと違う所) を先に見てください。

## まだ書けていないこと

- **マイコンごとの共通の部品。** できたら、このページはその使い方に書き直します。
- **ここに無いマイコン。** ESP32 を基板にする例、STM32 のほかの系列（CAN FD のコントローラを持つものなど）の例はありません。
- **書き込みの手順。** micro-MD の README には、WCH-Link で書き込む `pio run -t upload` があります。CAN_MD の README には、新しい基板に 1 回だけ書くオプションバイトの指定（`STM32_Programmer_CLI`）はありますが、ファームを書き込む手順はありません。

## このページについて

2026-11-15 に、仕様書の `board.md` の「ビルドへの組み込み」と既存のファームの一覧を材料に書きました。関数の名前と設定の値は、`examples/node_md_rp2040/src/main.cpp`、`firmware/micro-MD/src/main.c`、`firmware/CAN_MD/App/app.c`、`firmware/CAN_MD/Core/Src/main.c` と、各ファームの `platformio.ini`、`mas_node.py`、`CMakeLists.txt` を開いて突き合わせました。ビルドと実機では確かめていません。
