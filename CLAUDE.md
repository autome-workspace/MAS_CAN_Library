# MAS CAN Library

MAS CAN プロトコル v1 の再設計と、そのライブラリ・ファーム。**決定の正本は `docs/internal/decisions/` の台帳**（書き方はその README）。`docs/internal/archive/redesign/` は 2026-10-14 までの記録で凍結。版とブランチの方針は `docs/internal/versioning.md`。

- 決定は台帳にだけ書く。検討は `docs/internal/notes/` に日付付きで書き、書いた後は直さない。未決は `docs/internal/decisions/open.md`。
- 台帳と `docs/internal/notes/`、`docs/internal/archive/redesign/` は作者の私用。外部の人が読むのは `docs/spec/`（台帳から作った仕様書、2026-11-14〜）。**台帳、`lib/mas` の core のヘッダ、`node.h`、`node.c`、`host.cpp` を変えたら、同じコミットで `docs/spec/` を点検して直し、`python3 tools/spec_check.py --stamp` で記録する。** 引かない決定は `docs/spec/uncited.md` に理由を書く。仕組みは `docs/spec/README.md` の「保守」。
- docs の置き場所は読む人で分ける（2026-11-15、ユーザ）: `docs/guide/` 利用者向けの案内、`docs/spec/` 仕様書、`docs/internal/` 作者と直す人のもの。地図は `docs/README.md`。**`docs/guide/` と `docs/spec/` の文書を書く前に `docs/internal/writing.md` を読む。** 案内の各ページは、頭の枠（読む人、前提、読むとできること、見せないもの、材料）に沿って書く。
- ユーザは `docs/spec/` をスマホの artifact で読む（2026-11-14、ユーザ）。`docs/spec/` を直したら `python3 tools/spec_page.py <scratchpad>/mas-can-spec.html` で作り直し、同じ artifact に出し直す（中身を手で写さない）。
- `docs/guide/` と `docs/spec/` は部の Wiki（wiki.syamei0.com の `MAS/MAS-CAN/guide/…`、`spec/…`）にも載せている（2026-11-16、ユーザ）。`next` に入れて push した後に `python3 tools/wiki_sync.py <scratchpad>/wiki --upload --check` で作り直す（中身の変わったページだけ送る）。API キーは `~/.config/mas-can/wiki-token`。`MAS/MAS-CAN` そのもの（手で書いた要約）はスクリプトが触らない。置き換えるかは未決。

## 主制御側のライブラリ

`lib/mas` の主制御側（`Host`、`Device` と取っ手、`Port`、`Esp32Twai` など）は、~~ユーザが自分で空から書き直す（2026-10-14〜）~~ → **2026-10-15 から Claude が実装する**（ユーザ: 自分で実装するとも言っていられなくなった）。決定は台帳（`docs/internal/decisions/api.md`）に従い、台帳に無いことは決めずにユーザに聞く。書いたものは PC のテストで確かめる。
- 目標は `examples/api_tour/src/main.cpp` がビルドできること。**API の正典は `docs/internal/decisions/api.md`**（2026-10-14、ユーザ）。順番は台帳 → ヘッダ → 見本。
- `api_tour` の役目（2026-10-15、ユーザ）: (1) 決定済みの API を一通り呼び、ビルドが通ればヘッダが台帳と食い違っていないと分かる道具。(2) 使い心地を試す場所（見本にしか無いものは「試し」で、台帳に書いて決定になる）。部員に教える例にはしない（密度が高い。教える例は必要になったら小さく分けて作る）。
- 2026-10-14 までの経緯は `docs/internal/archive/redesign/library-api-draft.md`（凍結）。API の決定は `docs/internal/decisions/api.md` に書き、残りの保留は `docs/internal/decisions/open.md` にある。

## 作業場

作業はリポジトリの直下（`next`）だけで行う。worktree の `.ref/`（旧実装の写し）と `.mini/`（`mini` ブランチ）は 2026-10-15 に消した。旧実装はタグ `ref-claude-impl` に残っている。CAN-Servo のファームは `firmware/CAN-Servo-v1` に移した。

## テストとビルド

- PC のテスト: `make -C lib/mas/test`（`test_core`、`test_node`、`test_host`、`test_roundtrip`）。
- 例と RP2040 のファームのビルド: そのフォルダで `pio run`（`examples/` の各例、`firmware/CAN-Servo-v1`）。教える例は `md_minimal`、`md_check`（回らない理由を表示する。2026-11-15）、`servo_minimal`（主制御）、`node_md_rp2040`（基板側の雛形）。`md_serial_test` は試すための道具。例は増やさない（2026-11-15、ユーザ: 引き算で考える）。
- MD のファーム: `firmware/CAN_MD` を CMake で **Release** でビルドする（Debug は FLASH に入らない）。コンパイラは `~/.local/share/stm32cube/bundles/gnu-tools-for-stm32/*/bin`。

## 書き方

- 整形はリポジトリ直下の `.clang-format`（Google を土台にインデント 4）。CubeMX の生成物は触らない。
- ファイルの置き場所: `include/mas/can/` の直下は立場のもの（`host.h`、`device.h`、`port.h`、`node.h`）だけ。親は `xxx.h`、それを継承する側は `xxx/`（`device/`、`port/`）。`core/` は線の上の形（C、基板と共有）。経緯は library-api-draft.md の「取っ手の置き場所」「ファイルの置き場所」。
- コメントは要点だけ。説明は docs に書く。
- double は使わない。小数は float（32 bit。API-60、2026-10-15、ユーザ）。
- 決定は docs に日付と「ユーザ」を添えて書く。取り消した決定は消さずに取り消し線で残す。
