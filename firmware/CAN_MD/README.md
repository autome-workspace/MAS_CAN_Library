# CAN_MD

MD 1 ch（STM32F042、DRV8701）。

| | |
|---|---|
| ch | DIP で 0〜7。LED が ch の回数だけ点滅 |
| 指令 | duty、電流（速度は不可） |
| 電流の上限 | 15 A（既定も 15 A） |

## ビルドと書き込み

```sh
cmake --preset Release && cmake --build --preset Release
```

新しい基板には 1 回 `STM32_Programmer_CLI -c port=SWD -ob BOOT_SEL=0` を書く。

## 既知の制約

- HSI で動いている（CAN にはクロックの精度が足りない）。
- 電流の上限の既定が 15 A。
- 小さな電流の指令で duty 0.345 より下がらない。

ほかは [`TODO.md`](TODO.md)。
