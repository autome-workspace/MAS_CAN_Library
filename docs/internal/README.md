# internal: 作者と、直す人のもの

利用者は読まなくて構いません。使い方は [../guide/](../guide/)、決まりは [../spec/](../spec/) にあります。

| 場所 | 中身 | 決まり |
|---|---|---|
| [decisions/](decisions/) | 決定の台帳。**仕様の正本** | 書き方は [decisions/README.md](decisions/README.md)。決定はここにだけ書く |
| [notes/](notes/) | 検討、比較、点検のメモ（日付つき） | 書いた後は直さない |
| [versioning.md](versioning.md) | 版とブランチの方針 | |
| [writing.md](writing.md) | 説明の文書（guide、spec）の書き方 | 説明の文書を書く前に読む |
| [archive/redesign/](archive/redesign/) | v1 を設計したときの記録（2026-10-14 に凍結） | 直さない。食い違ったら台帳が正しい |

直すときの手順:

- 台帳やライブラリの core を変えたら、`python3 tools/spec_check.py` で仕様書の点検が要る所を出す（仕組みは [../spec/README.md](../spec/README.md) の「保守」）。
- PC のテストは `make -C lib/mas/test`。
- AI に作業させるときの決まりは、リポジトリ直下の `CLAUDE.md`。
