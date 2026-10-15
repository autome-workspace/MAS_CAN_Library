# MAS CAN Library

MAS CAN プロトコル v1 の再設計と、そのライブラリ・ファーム。**決定の正本は `docs/decisions/` の台帳**（書き方はその README）。`docs/redesign/` は 2026-10-14 までの記録で凍結。版とブランチの方針は `docs/versioning.md`。

- 決定は台帳にだけ書く。検討は `docs/notes/` に日付付きで書き、書いた後は直さない。未決は `docs/decisions/open.md`。

## 主制御側のライブラリは、ユーザが自分で書く

`lib/mas` の主制御側（`Host`、`Device` と取っ手（`Md`、`Controller`、`Pin`、`Air`、`Solenoid`）、`Port`、`Esp32Twai` など）は、部で共有するので、ユーザが中身を完全に把握するために**自分で空から書き直している**（2026-10-14〜）。

- **Claude は補助に回る。代わりに書かない。** 一歩ずつ「どのファイルのどこに、何を、なぜ書くか」を前提から説明し、ユーザが書いたものをレビューし、テストで確かめる。Claude が書くのは、ユーザに頼まれた部分だけ。
- 目標は `examples/api_tour/src/main.cpp` がビルドできること。**API の正典は `docs/decisions/api.md`**（2026-10-14、ユーザ）。順番は台帳 → ヘッダ → 見本。使用感を見るために見本で先に試してもよいが、見本にしか無いものは「試し」で、台帳に書いて決定になる。
- 2026-10-14 までの経緯は `docs/redesign/library-api-draft.md`（凍結）。API の決定は `docs/decisions/api.md` に書き、残りの保留は `docs/decisions/open.md` にある。

## 作業場（git worktree）

| 場所 | ブランチ | 中身 |
|---|---|---|
| リポジトリの直下 | `next` | ユーザが書き直す場所 |
| `.ref/` | タグ `ref-claude-impl` | Claude が書いた旧実装の写し。**読むだけ**（編集しても `next` に入らない） |
| `.mini/` | `mini` | 部ですぐ使う最低限の版（Claude が書いた。API は同じ、中身は固定の配列）。CAN-Servo 基板の v1 のファームもここ。説明は `mini` の `docs/mini.md` |

`.ref/` と `.mini/` は `.gitignore` 済み。`mini` は答え合わせに使えるが、写すと自分で把握する目的から外れるので、見るなら書いた後。

## テストとビルド

- PC のテスト: `make -C lib/mas/test`。基板側のコア（`test_core`、`test_node`）は通る。主制御側のテストは `test_host.cpp` として書く予定（旧版は `.ref/lib/mas/test/test_bus.cpp`。旧版の `Bus` は今の `Host`）。
- 例のビルド: `examples/<名前>` で `pio run`。

## 書き方

- 整形はリポジトリ直下の `.clang-format`（Google を土台にインデント 4）。CubeMX の生成物は触らない。
- ファイルの置き場所: `include/mas/can/` の直下は立場のもの（`host.h`、`device.h`、`port.h`、`node.h`）だけ。親は `xxx.h`、それを継承する側は `xxx/`（`device/`、`port/`）。`core/` は線の上の形（C、基板と共有）。経緯は library-api-draft.md の「取っ手の置き場所」「ファイルの置き場所」。
- コメントは要点だけ。説明は docs に書く。
- 決定は docs に日付と「ユーザ」を添えて書く。取り消した決定は消さずに取り消し線で残す。
