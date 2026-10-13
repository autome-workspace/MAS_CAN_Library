# MAS CAN Library

MAS CAN プロトコル v1 の再設計と、そのライブラリ・ファーム。記録は `docs/redesign/`、版とブランチの方針は `docs/versioning.md`。

## 主制御側のライブラリは、ユーザが自分で書く

`lib/mas` の主制御側（`Bus`、`Md`、`Controller`、`Port`、`Esp32Twai` など）は、部で共有するので、ユーザが中身を完全に把握するために**自分で空から書き直している**（2026-10-14〜）。

- **Claude は補助に回る。代わりに書かない。** 一歩ずつ「どのファイルのどこに、何を、なぜ書くか」を前提から説明し、ユーザが書いたものをレビューし、テストで確かめる。Claude が書くのは、ユーザに頼まれた部分だけ。
- 目標は `examples/api_tour/src/main.cpp` がビルドできること。**この見本が API の正本**で、ヘッダは見本に合わせる。
- 決定の経緯は `docs/redesign/library-api-draft.md` の「API を固める」節。保留の論点（`waitReady`、刻みの既定値の書き込み、loop の中の確保の失敗、A9〜A11、`stop`／`commandConflict`／`onSettingError`、Q34）は、ユーザが実装しながら決め、決めたら同じ節の表に書き足す。

## 作業場（git worktree）

| 場所 | ブランチ | 中身 |
|---|---|---|
| リポジトリの直下 | `next` | ユーザが書き直す場所 |
| `.ref/` | タグ `ref-claude-impl` | Claude が書いた旧実装の写し。**読むだけ**（編集しても `next` に入らない） |
| `.mini/` | `mini` | 部ですぐ使う最低限の版（Claude が書いた。API は同じ、中身は固定の配列）。CAN-Servo 基板の v1 のファームもここ。説明は `mini` の `docs/mini.md` |

`.ref/` と `.mini/` は `.gitignore` 済み。`mini` は答え合わせに使えるが、写すと自分で把握する目的から外れるので、見るなら書いた後。

## テストとビルド

- PC のテスト: `make -C lib/mas/test`。基板側のコア（`test_core`、`test_node`）は通る。主制御側のテスト（`test_bus`）は消してあり、旧版は `.ref/lib/mas/test/test_bus.cpp`。
- 例のビルド: `examples/<名前>` で `pio run`。

## 書き方

- 整形はリポジトリ直下の `.clang-format`（Google を土台にインデント 4）。CubeMX の生成物は触らない。
- コメントは要点だけ。説明は docs に書く。
- 決定は docs に日付と「ユーザ」を添えて書く。取り消した決定は消さずに取り消し線で残す。
