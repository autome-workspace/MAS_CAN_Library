# internal: 作者と、直す人のもの

利用者は読まなくて構いません。使い方は [../guide/](../guide/)、決まりは [../spec/](../spec/) にあります。

## 何が、どこにあるか

| 場所 | 中身 | 決まり |
|---|---|---|
| [decisions/](decisions/) | 決定の台帳。**仕様の正本** | 書き方は [decisions/README.md](decisions/README.md)。決定はここにだけ書く |
| [notes/](notes/) | 検討、比較、点検のメモ（日付つき） | 書いた後は直さない。足すのはよい |
| [versioning.md](versioning.md) | 版とブランチの方針 | |
| [writing.md](writing.md) | 説明の文書（案内、仕様書）の書き方 | 説明の文書を書く前に読む |
| [archive/redesign/](archive/redesign/) | v1 を設計したときの記録（2026-10-14 に凍結） | 直さない。食い違ったら台帳が正しい |

文書どうしの関係は、こうなっています。

- **台帳**が正本です。線の上の形と、ライブラリの関数の形は、台帳の 1 行 1 行が決めています。
- **仕様書**（`docs/spec/`）は、台帳から作った、読むための文書です。食い違ったら、台帳が正しいです。
- **案内**（`docs/guide/`）は、使い方です。台帳と、コードと、例（`examples/`）に突き合わせて書きます。

## 今の状態（2026-11-15）

- **v1 が正式版です**（`v1.0.0`）。部で使われています。
- **v2（互換の切れる作り直し）は、検討して、保留にしました。** 先に、v1 の上で、使う人が困っている所を直します。検討の記録は notes の 4 つです。読むなら [notes/2026-11-15-v2-roots.md](notes/2026-11-15-v2-roots.md) だけで足ります（目的、使う人、優先順位、保留にした理由、戻るきっかけ）。`v2-proposal`、`v2-scenes`、`v2-draft` は、そこに至る途中の案です。
- **説明の文書を組み直しています。** 方針は [notes/2026-11-15-docs-rebuild-plan.md](notes/2026-11-15-docs-rebuild-plan.md) にあります。

## やり残しは、どこを見るか

| 見る場所 | 中身 |
|---|---|
| [decisions/open.md](decisions/open.md) | まだ決めていないこと。取り消し線の行は、決まって台帳に移ったものです。生きているのは、取り消し線の無い行と、下のほうのメモの節です |
| [notes/2026-11-14-spec-audit.md](notes/2026-11-14-spec-audit.md) | 実装が台帳どおりでない所（A）と、台帳で決まっていない所（B）の一覧。いちばん新しい点検です |
| [notes/2026-11-15-read-api-gaps.md](notes/2026-11-15-read-api-gaps.md) | 動かない理由を調べるための、読む関数の足りない所 |
| [versioning.md](versioning.md) の「未定」 | 版まわりの、まだ決めていないこと |
| `firmware/CAN_MD/TODO.md` | CAN_MD のファームの直す点 |
| [../guide/wiring.md](../guide/wiring.md) と各ページの「まだ書けていないこと」 | 文書で、実物を確かめないと書けない所 |

## 直すときの手順

1. **決まりを変えるなら、先に台帳に書きます。** 順番は、台帳、ヘッダ、実装、テスト、文書です。
2. **PC のテストを走らせます。** `make -C lib/mas/test`。4 つとも `passed` と出れば通っています。
3. **台帳か、ライブラリの core、`node.h`、`node.c`、`host.cpp` を変えたら、仕様書を点検します。** `python3 tools/spec_check.py` が、点検の要る所を出します。直したら `python3 tools/spec_check.py --stamp` で記録します。仕組みは [../spec/README.md](../spec/README.md) の「保守」にあります。
4. **コミットの前の点検を有効にします。** クローンごとに 1 回、`git config core.hooksPath .githooks` を実行します。点検が通らないコミットを止めます。
5. **例とファームをビルドします。**
   - 例（`examples/`）と、RP2040 と CH32 のファーム（`firmware/CAN-Servo-v1`、`firmware/xdrive-relay`、`firmware/micro-MD`）: そのフォルダで `pio run`。
   - `firmware/CAN_MD`: CMake で、Release でビルドします（Debug は FLASH に入りません）。手順は `firmware/CAN_MD/README.md` にあります。
6. **ブランチ。** `next` には、いつ `main` に入れてもよいものだけを置きます。実機で確かめる必要のある変更は、短い作業用のブランチで進め、確かめてから `next` に入れます。`main` には `next` を早送りで入れ、直接コミットしません（[versioning.md](versioning.md) の V8）。

AI に作業させるときの決まりは、リポジトリ直下の `CLAUDE.md` にあります。

## 台帳を、どこから読むか

台帳は 4 つで、500 行ほどあります。全部を先に読む必要はありません。

| ファイル | 中身 | 先に見る所 |
|---|---|---|
| [decisions/common.md](decisions/common.md) | 全種類に共通の決まり | 「前提」の節（何を目指し、何をしないと決めたか） |
| [decisions/md.md](decisions/md.md) | モータードライバー | 「動作」の節 |
| [decisions/servo.md](decisions/servo.md) | サーボ | 短いので全部 |
| [decisions/api.md](decisions/api.md) | 主制御のライブラリの関数の形 | 「送受信のタイミング」と「プログラムの値と再起動」の節 |

決まりの背景を知りたいときは、先に仕様書の [overview.md](../spec/overview.md) と [flows.md](../spec/flows.md) を読むほうが早いです。台帳は、理由を 1 行でしか書いていません。
