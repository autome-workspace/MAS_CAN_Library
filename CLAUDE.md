# MAS CAN Library

MAS CAN プロトコル v1 の再設計と、そのライブラリ・ファーム。**決定の正本は `docs/decisions/` の台帳**（書き方はその README）。`docs/redesign/` は 2026-10-14 までの記録で凍結。版とブランチの方針は `docs/versioning.md`。

- 決定は台帳にだけ書く。検討は `docs/notes/` に日付付きで書き、書いた後は直さない。未決は `docs/decisions/open.md`。

## 主制御側のライブラリ

`lib/mas` の主制御側（`Host`、`Device` と取っ手、`Port`、`Esp32Twai` など）は、~~ユーザが自分で空から書き直す（2026-10-14〜）~~ → **2026-10-15 から Claude が実装する**（ユーザ: 自分で実装するとも言っていられなくなった）。決定は台帳（`docs/decisions/api.md`）に従い、台帳に無いことは決めずにユーザに聞く。書いたものは PC のテストで確かめる。
- 目標は `examples/api_tour/src/main.cpp` がビルドできること。**API の正典は `docs/decisions/api.md`**（2026-10-14、ユーザ）。順番は台帳 → ヘッダ → 見本。
- `api_tour` の役目（2026-10-15、ユーザ）: (1) 決定済みの API を一通り呼び、ビルドが通ればヘッダが台帳と食い違っていないと分かる道具。(2) 使い心地を試す場所（見本にしか無いものは「試し」で、台帳に書いて決定になる）。部員に教える例にはしない（密度が高い。教える例は必要になったら小さく分けて作る）。
- 2026-10-14 までの経緯は `docs/redesign/library-api-draft.md`（凍結）。API の決定は `docs/decisions/api.md` に書き、残りの保留は `docs/decisions/open.md` にある。

## 作業場（git worktree）

| 場所 | ブランチ | 中身 |
|---|---|---|
| リポジトリの直下 | `next` | ユーザが書き直す場所 |
| `.ref/` | タグ `ref-claude-impl` | Claude が書いた旧実装の写し。**読むだけ**（編集しても `next` に入らない） |

`.ref/` は `.gitignore` 済み。`mini`（`.mini/`）は 2026-10-15 に消した（CAN-Servo のファームは `firmware/CAN-Servo-v1` に移した）。

## テストとビルド

- PC のテスト: `make -C lib/mas/test`（`test_core`、`test_node`、`test_host`）。
- 例と RP2040 のファームのビルド: そのフォルダで `pio run`（`examples/api_tour`、`examples/md_serial_test`、`firmware/CAN-Servo-v1`）。
- MD のファーム: `firmware/CAN_MD` を CMake で **Release** でビルドする（Debug は FLASH に入らない）。コンパイラは `~/.local/share/stm32cube/bundles/gnu-tools-for-stm32/*/bin`。

## 書き方

- 整形はリポジトリ直下の `.clang-format`（Google を土台にインデント 4）。CubeMX の生成物は触らない。
- ファイルの置き場所: `include/mas/can/` の直下は立場のもの（`host.h`、`device.h`、`port.h`、`node.h`）だけ。親は `xxx.h`、それを継承する側は `xxx/`（`device/`、`port/`）。`core/` は線の上の形（C、基板と共有）。経緯は library-api-draft.md の「取っ手の置き場所」「ファイルの置き場所」。
- コメントは要点だけ。説明は docs に書く。
- double は使わない。小数は float（32 bit。API-60、2026-10-15、ユーザ）。
- 決定は docs に日付と「ユーザ」を添えて書く。取り消した決定は消さずに取り消し線で残す。
