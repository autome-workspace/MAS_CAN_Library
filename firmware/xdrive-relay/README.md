# xdrive-relay

XDrive を主バスから切り離す中継機（RP2040）のファーム。

- 主バス（MAS CAN v1、1 Mbps）では、速度の指令を受ける 4 ch の MD に見える（M-4）。
- 下のバス（XDrive の CANSimple）で、XDrive の axis を ch ごとに 1 個動かす。

2 本のバスは ID がぶつかるので、必ず分ける（MAS の SYNC `0x002` は、XDrive の node 0 から見ると Estop）。

## 配線と設定

`platformio.ini` の `build_flags` で変える。

| | 既定 | |
|---|---|---|
| `MAS_TX` / `MAS_RX` | 0 / 1 | 主バス（PIO1）。基板の CAN1 |
| `XD_TX` / `XD_RX` | 3 / 2 | 下のバス（PIO0）。基板の CAN2（CAN1 と TX・RX の並びが逆） |
| `MD_FIRST_CH` | 0 | 受け持つ ch の先頭（4 の倍数） |
| `XD_NODE0`〜`3` | 0〜3 | ch ごとの XDrive の `can_node_id`。使わない ch は 255 |
| `XD_BAUD` | 250000 | XDrive の `can.config.baud_rate` と同じにする |
| `XD_KT` | 0.014768 | [Nm/A] XDrive の `motor.config.torque_constant` と同じにする |
| `XD_VEL_LIMIT` | 18.0 | [turn/s] モータ軸。XDrive の `controller.config.vel_limit` 以下にする |
| `XD_CURRENT_LIMIT_MA` | 12000 | XDrive の `motor.config.current_lim`。設定 33 の上限になる |

XDrive の側では、odrivetool で次を済ませておく。

- 校正（`pre_calibrated`）、`can_node_id`、`baud_rate`。
- watchdog を有効にする: `axis0.config.enable_watchdog = True`、`axis0.config.watchdog_timeout = 0.2`。中継機が止まったり線が抜けたりしたら、0.2 秒で XDrive が自分で止まる。中継機は、Idle の間も問い合わせを送り続けて watchdog に餌をやる。

## ビルドと書き込み

```sh
pio run -t upload                  # 本番
pio run -e pico_debug -t upload    # USB シリアルに 1 秒ごとの状態を出す
make -C test                       # PC のテスト
```

`pico_debug` では、主制御が無くてもシリアルから指令を入れられる。

| | |
|---|---|
| `v <ch> <rpm>` | 速度の指令を 10 ms ごとに入れ続ける（出力軸の rpm） |
| `c <ch> <mA>` | 電流の指令 |
| `x` | 入れるのをやめる（100 ms で途絶 → 止まる） |
| `e` / `r` | E-stop / 解除 |
| `f` | 異常の詳細を消す（XDrive の Fault の解除） |
| `w <ch> <item> <値>` | 設定を書く（例: `w 0 37 19.2` で減速比） |

## 主バスの指令と XDrive の動き

| 主バス | XDrive |
|---|---|
| 速度 [rpm] | 速度制御。モータ軸の turn/s ＝ rpm ÷ 60 × 減速比（設定 37）。最高速度 45 と最大加速度 46 は中継機で掛ける |
| 電流 [mA] | トルク制御。Nm ＝ mA ÷ 1000 × `XD_KT`。設定 33 で抑える |
| duty | XDrive に無い。こなせない指令として止める（M-30、FB の bit 4） |
| 止める・空転 | Idle |
| 止める・ブレーキ | 回っていれば速度 0 で保持。Idle なら Idle のまま（止まっている XDrive を Closed loop に入れない） |
| E-stop | 回っていれば速度 0、止まったら（0.2 turn/s 未満か 1 秒）Idle |

- 速度と電流は、走っている最中でも Closed loop のまま切り替える。
- FB の位置は、起動して最初に読めた位置を 0 にする（M-2）。出力軸 1 回転 ＝ 65536。
- 反転（32）は MAS のライブラリ（node）が指令と FB の両方に掛ける。
- 下のバスの周期: 250 kbps では 20 ms（指令と位置・速度）、40 ms（電流）。500 kbps 以上ではその半分。4 台で使用率はおよそ 35〜40%。

## XDrive の異常

| 起きたこと | 中継機 |
|---|---|
| XDrive がエラーを出した / 回している最中に Closed loop から外れた / 3 秒で Closed loop に入らない | XDrive に Estop を送り、Fault。止まったのを確かめてから、motor と encoder のエラーを読む |
| Heartbeat が 0.3 秒来ない | Fault。FB の速度・位置・電流を無効にする（M-18 の bit 1、2） |

- Fault の間は、FB の bit 4（止めている）と、異常の詳細（M-44: 8 過電流、9 過熱、10 低電圧、11 過電圧、12 エンコーダ、13 ゲートドライバ）を立てる。
- **表に当てはまらない Fault は bit 31**（その他）を立てる。OVERSPEED（axis の `CONTROLLER_FAILED`。CANSimple では controller のエラーを読めない）、エラー無しで Closed loop から外れた、Closed loop に入らない、Heartbeat の途絶、など。詳細が 0 のままだと、主制御に FAULT の bit が見えず、ライブラリの `clearFault()` も何も送らない（E-stop の解除でしか解けなかった）。
- **直せる原因の Fault は、中継機が自分で戻す。主制御は何もしなくてよい**（下の表）。XDrive が 0.5 秒続けて見えていて Closed loop でない（止まっていると確かめられる）とき、`clear_errors` を送って Idle に戻す。指令が続いていれば、そのまま回り出す。回数の上限は無い（CAN の線のガタつきや電源の瞬断で、XDrive に不都合が無いのに止まったままにしない）。
- **それ以外の Fault は解除を待つ。** 解くのは、主制御が異常の詳細（設定 5）を書いたとき、または E-stop の解除のとき。原因が残っていれば、次に回そうとしたときにまた Fault になる。
- 異常の詳細の bit 8〜31 は「今 Fault の ch の原因」。戻れば中継機が下ろす（主制御のライブラリは、FB の FAULT の bit が消えると手元の詳細を 0 に戻す）。4 ch で 1 つを共有する。

| 原因 | 自分で戻すか |
|---|---|
| Heartbeat の途絶 → 戻った（XDrive の電源・CAN が切れた）。戻った XDrive のエラーが watchdog・エンコーダ（起動直後）・低電圧・Estop の跡だけ | 戻す |
| エラー無しで Closed loop から外れた（瞬断で再起動した） | 戻す |
| 低電圧（`DC_BUS_UNDER_VOLTAGE`）、`CONTROL_DEADLINE_MISSED` だけ、前の中継機が送った Estop の跡だけ | 戻す |
| OVERSPEED（`CONTROLLER_FAILED`）、過電流、ゲートドライバ、過熱、過電圧、エンコーダ（回している最中）、Closed loop に入らない、校正が無い | 解除を待つ |
- XDrive の `WATCHDOG_TIMER_EXPIRED` だけは、回し始めるときに中継機が自分で消す（中継機より先に XDrive が起きていれば必ず立っている）。
- `ENCODER_FAILED` だけが立っているときは、回し始めるときに 1 度だけ消して、次の Heartbeat で見直す。XDrive の起動直後は、絶対値 SPI の最初の読み出しが失敗して必ず立っている（`ABS_SPI_COM_FAIL`。2026-10-27 に 2 台で確かめた）。本当に読めなければ、Closed loop の要求でまた立ち、Fault（bit 12）になる。
- XDrive を見てから 1 秒は回さない。起きたばかりの XDrive に Closed loop を求めると、`CONTROLLER_FAILED` と `CONTROL_DEADLINE_MISSED` で落ちる（2026-10-28、最初の Heartbeat から 0.1 s で求めて 4 台とも）。
- 中継機が起きて初めて見た XDrive のエラー（前の中継機の Estop の跡など）は、1 度だけ消す。
- 下のバスで、渡したフレームが 30 ms 1 本も通らなければ、can2040 を止めて送り待ちを捨てる（`main.cpp` の `xdWatch`）。can2040 は ACK の来ないフレームを止めずに送り直すので、XDrive が全部消えると宛先の無い Estop を毎秒 2000 本ほど送り続け、戻ってきた XDrive がその中で起きて CAN が止まった。
- **XDrive（ODrive fw 0.5.x）の CAN は、ドライバにエラーが付くと再起動まで送受信をやめる**（`interface_can.cpp` の `can_server_thread` は timeout 以外のエラーを立て直さない。0.5.4・0.5.6 も同じで、0.5.4 以降はエラーを消す処理も無い）。CAN の抜き差しやノイズで、その XDrive は電源を入れ直すまで見えなくなることがある。中継機からは直せない。直すなら XDrive の fw に手を入れる（XDrive の fw は版 0.0.0 の独自のビルドなので、ソースを確かめてから）。
- FB の電流と bit 3（電流の上限）は、XDrive が Closed loop の間だけ。Idle の XDrive は Iq を更新せず、最後の値（たとえば 12 A）を返し続ける。

## 設定項目のうち、XDrive ではできないもの

| 項目 | 返す結果 |
|---|---|
| 36 CPR | 受け付けるが使わない（XDrive が自分のエンコーダを持つ） |
| 38 エンコーダの向き | 4（M-41: 向きは転流と一体） |
| 39〜41 速度ループのゲイン | 4。XDrive の `vel_gain`、`vel_integrator_gain` を odrivetool で合わせる |

## チームで決めること

ここでは仮に決めた。

- **duty の指令**: こなせない指令として止めている。
- **ブレーキ**: 「速度 0 で保持」にした。E-stop（M-51 は常にブレーキ）は、止まったら Idle にする。
- **Fault の解き方**: 電源の入れ直し・瞬断・低電圧・制御周期の遅れは中継機が自分で戻す（主制御に解除を求めない）。暴走・過電流など原因が残りうるものは、設定 5 の書き込みか E-stop の解除。どれを自分で戻すかは仮。
- **異常の詳細の bit 31（その他）**: M-44 では bit 14〜31 は予約。表に無い Fault を主制御に見せるために仮に使っている。MAS の仕様で「その他」の bit を取るか決める。
- **速度の指令のときの電流の上限**: 設定 33 は、電流の指令にだけ効く。速度の指令のときに効くのは、XDrive の `current_lim` だけ（fw 0.5.1 の CAN からは変えられない）。
- **基板の種類の番号**: いまは `MAS_CAN_BOARD_RESERVED`。MAS の `core/board.h` に番号を取る。

## 実機での確認（2026-10-21）

主制御（ESP32、`scratch/xdrive-relay-host`）→ 中継機 → XDrive 1 台で確かめた。

- 速度と電流の指令で回る。走っている最中の種類の切り替えも通る。
- 主制御の側から 4 ch とも online。XDrive の無い ch は「速度・電流が無効、止めている」に見える。
- XDrive を抜くと、その ch は 0.3 秒で Fault（Heartbeat が途絶えた）。別の node の XDrive を差すと、その ch で普通に回る。
- 下のバスの受信エラーは、回している間 0。抜き差しの間だけ出る。

主制御の側の注意: 4 ch は 1 本のフレームで送るので、ch ごとに種類（速度・電流）を混ぜられない（MAS のライブラリの API-52。混ぜると、いちばん小さい ch の種類しか送られず、ほかは途絶で止まる）。

## 実機での確認（2026-10-27）

主制御から速度で回すと、ch1・ch3 だけ一瞬（1 回転ほど）で止まり、`stopped limit`（12 A）のまま、`cur 0` も `clrfault` も効かなかった。

- 原因は XDrive の側。ch1・ch3 のエンコーダの offset が電気角で 100° 以上ずれていた（ch1: 15743 → 23487、ch3: 21890 → 16531）。90° を超えるとトルクが逆に効いて暴走し、OVERSPEED（22 turn/s ＝ vel_limit × 1.2）で落ちる。`AXIS_STATE_ENCODER_OFFSET_CALIBRATION` で取り直し、±10 turn/s で追従を確かめて保存した。2 回取って 1 カウント違いで再現する。
- 両台とも `enable_watchdog` が False だった。True / 0.2 にして保存した。
- 中継機の側の見え方の問題を 3 つ直した（上の「XDrive の異常」: bit 31、起動直後の `ENCODER_FAILED`、Idle の Iq）。`test/` に再現のテストがある。
- 取り直しの途中で 2 回だけ `CONTROL_DEADLINE_MISSED`（XDrive が自分で Closed loop から外れる）が出た。直した後は、60 秒の保持と 22 回の切り替えで出ていない。

## 実機での確認（2026-10-28）

主制御（ESP32、md_console）→ 中継機 → XDrive 4 台。主制御は何もしない（`clrfault` も E-stop の解除も打たない）。

| | やったこと | 結果 |
|---|---|---|
| T1 | 回したまま XDrive の主電源を全部切り、10 秒後に入れる | 入れてから約 1 秒で 4 台とも running、回り出す |
| T3 | 主電源を 1 秒だけ切る | 3 秒で 4 台とも running |
| T4 | 中継機の USB を抜いて 5 秒後に挿す | 4 台とも running、主制御からも online |
| T5 | 回したまま 1 台の CAN を 3 秒抜く（何度も） | その 1 台だけ止まり、毎回自分で戻る。ほかは回り続ける |

- **電源**: XDrive 4 台を 1.2 A までの安定化電源で動かすと、待機と突入電流だけで上限に張り付き、回すと電圧が落ちて 4 台とも CAN から消えた。起動しない（LED は付くが USB にも CAN にも出ない）こともあった。電源を替えたら起きなくなった。
- `pico_debug` では、シリアルの読み手が詰まると書き込みで中継が止まり、全 ch が heartbeat-lost になった。1 行ずつ空きを確かめて書く（最長 5 ms 待つ）ようにした。`pico`（本番）はシリアルに書かない。
- USB を挿したまま XDrive の主電源を切ると、USB からの回り込みで XDrive が中途半端に生き、バスを乱した疑いがある。主電源を切るときは XDrive の USB を抜く。

