# 例

上から順に使います。

| 例 | 何をするか | いつ使うか |
|---|---|---|
| [`md_minimal`](md_minimal/) | モーターを 1 つ回す | 最初に。手順は [はじめて回す](../docs/guide/host/first-run.md) |
| [`md_check`](md_check/) | モーターを 1 つ回しながら、回らない理由を 1 語で表示する | 回らないとき。言葉の意味は [動かないとき](../docs/guide/host/troubleshooting.md) |
| [`servo_minimal`](servo_minimal/) | サーボを 1 つ動かす | サーボを使うとき |

次の 2 つは、基板を作る人のものです。

| 例 | 何をするか |
|---|---|
| [`node_md_rp2040`](node_md_rp2040/) | 基板の側のファームの雛形。使い方は [雛形から始める](../docs/guide/board/start.md) |
| [`md_serial_test`](md_serial_test/) | シリアルから命令を送って、基板を試す道具。使い方は [確かめ方](../docs/guide/board/test.md) |

`api_tour` は例ではありません。ライブラリの関数を全部呼んで、ビルドが通るかを確かめるための道具です。まだ使えない関数も呼んでいるので、手本にしないでください。
