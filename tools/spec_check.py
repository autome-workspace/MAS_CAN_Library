#!/usr/bin/env python3
"""docs/spec/（外部向けの仕様書）が、台帳とコードに追いついているかを確かめる。

    python3 tools/spec_check.py           点検する。問題があれば 1 で終わる
    python3 tools/spec_check.py --stamp   点検し終えたことを docs/spec/reviewed.txt に記録する

確かめること:
  1. 引用: 仕様書が引く台帳の番号（C-46 など）が、台帳にあり、取り消されていない
  2. 網羅: 台帳（COVER）の今の決定が、仕様書に引かれているか、docs/spec/uncited.md に理由つきで載っている
  3. 鮮度: 元（WATCH）が、最後に点検した時から変わっていない。変わっていれば差分を出す
  4. コード: core/board.h の基板の種類とプロトコルの版が、仕様書に載っている
  5. リンク: 仕様書の相対リンクの先がある
"""
import difflib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "docs/spec"
STAMP = SPEC / "reviewed.txt"
UNCITED = SPEC / "uncited.md"
LEDGER = ROOT / "docs/decisions"

# 仕様書が全部を扱う台帳
COVER = ["docs/decisions/common.md", "docs/decisions/md.md", "docs/decisions/servo.md"]
# 変わったら仕様書を点検し直す元
WATCH = COVER + [
    "docs/decisions/api.md",
    "lib/mas/include/mas/can/node.h",
    "lib/mas/src/can/node.c",  # flows.md、board.md が「今の実装」を書く
    "lib/mas/src/can/host.cpp",
    "lib/mas/include/mas/can/core/board.h",
    "lib/mas/include/mas/can/core/frame.h",
    "lib/mas/include/mas/can/core/md_frame.h",
    "lib/mas/include/mas/can/core/setting.h",
]
BOARD_H = ROOT / "lib/mas/include/mas/can/core/board.h"

NUM = r"(?:C|M|S|API)-\d+"
ROW_CURRENT = re.compile(rf"^\|\s*({NUM})\s*\|")
ROW_RETIRED = re.compile(rf"^\|\s*~~({NUM})~~\s*\|")
CITE = re.compile(rf"(?<![\w-])({NUM})(?![\w-])")
LINK = re.compile(r"\]\(([^)#\s]+)(?:#[^)]*)?\)")


def ledger(files):
    current, retired = {}, set()
    for rel in files:
        for line in (ROOT / rel).read_text(encoding="utf-8").splitlines():
            if m := ROW_CURRENT.match(line):
                current[m.group(1)] = rel
            elif m := ROW_RETIRED.match(line):
                retired.add(m.group(1))
    return current, retired


def spec_files():
    return sorted(p for p in SPEC.glob("*.md") if p.name != UNCITED.name)


def cited():
    found = {}
    for p in spec_files():
        for m in CITE.finditer(p.read_text(encoding="utf-8")):
            found.setdefault(m.group(1), p.name)
    return found


def uncited_list():
    if not UNCITED.exists():
        return {}
    out = {}
    for line in UNCITED.read_text(encoding="utf-8").splitlines():
        if m := re.match(rf"^\|\s*({NUM})\s*\|\s*(.*?)\s*\|", line):
            out[m.group(1)] = m.group(2)
    return out


def git(*args, input=None):
    return subprocess.run(["git", "-C", str(ROOT), *args], input=input,
                          capture_output=True, text=True)


def blob(rel, write=False):
    args = ["hash-object"] + (["-w"] if write else []) + [rel]
    return git(*args).stdout.strip()


def read_stamp():
    if not STAMP.exists():
        return {}
    out = {}
    for line in STAMP.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            h, rel = line.split(None, 1)
            out[rel] = h
    return out


def changed_numbers(old, new):
    nums = set()
    for line in difflib.unified_diff(old.splitlines(), new.splitlines(), lineterm="", n=0):
        body = line[1:]
        if line[:1] in "+-" and not line.startswith(("+++", "---")) and body.startswith("|"):
            nums.update(CITE.findall(body.split("|")[1]))  # 行の番号の欄だけ
    return nums


def check():
    errors, notes = [], []
    all_files = sorted(str(p.relative_to(ROOT)) for p in LEDGER.glob("*.md"))
    current, retired = ledger(all_files)
    cover_current, _ = ledger(COVER)
    cites = cited()
    skip = uncited_list()

    # 1. 引用
    for n, where in sorted(cites.items()):
        if n in retired:
            errors.append(f"引用: {where} が取り消された {n} を引いている")
        elif n not in current:
            errors.append(f"引用: {where} が台帳に無い {n} を引いている")

    # 2. 網羅
    for n in sorted(cover_current, key=sort_key):
        if n not in cites and n not in skip:
            errors.append(f"網羅: {n}（{cover_current[n]}）が仕様書に無い。引くか uncited.md に理由を書く")
    for n, why in skip.items():
        if n in cites:
            errors.append(f"網羅: {n} は仕様書に引かれているのに uncited.md にもある")
        elif n not in current:
            errors.append(f"網羅: uncited.md の {n} は台帳の今の決定ではない")
        elif not why:
            errors.append(f"網羅: uncited.md の {n} に理由が無い")

    # 3. 鮮度
    stamp = read_stamp()
    for rel in WATCH:
        old = stamp.get(rel)
        new = blob(rel)
        if old == new:
            continue
        if old is None:
            errors.append(f"鮮度: {rel} を点検した記録が無い")
            continue
        prev = git("cat-file", "blob", old)
        if prev.returncode != 0:
            errors.append(f"鮮度: {rel} が点検の後に変わった（前の版が手元に無いので差分は出せない）")
            continue
        now = (ROOT / rel).read_text(encoding="utf-8")
        nums = changed_numbers(prev.stdout, now)
        what = f"変わった決定: {', '.join(sorted(nums, key=sort_key))}" if nums else "差分は git diff で見る"
        errors.append(f"鮮度: {rel} が点検の後に変わった（{what}）")
        diff = difflib.unified_diff(prev.stdout.splitlines(), now.splitlines(),
                                    f"点検した版 {old[:8]}", "今", lineterm="", n=0)
        notes.extend("    " + l for l in list(diff)[:40])

    # 4. コード
    text = "\n".join(p.read_text(encoding="utf-8") for p in spec_files())
    btext = BOARD_H.read_text(encoding="utf-8")
    for name, val in re.findall(r"#define MAS_CAN_BOARD_(\w+)\s+0x([0-9A-Fa-f]{4})u", btext):
        if name != "TYPE" and not re.search(rf"0x{val}(?![0-9A-Fa-f])", text, re.I):
            errors.append(f"コード: 基板の種類 0x{val}（MAS_CAN_BOARD_{name}）が仕様書に無い")
    if m := re.search(r"#define MAS_CAN_PROTOCOL_VERSION\s+0x([0-9A-Fa-f]{8})u", btext):
        if not re.search(rf"0x{m.group(1)}", text, re.I):
            errors.append(f"コード: プロトコルの版 0x{m.group(1)}（board.h）が仕様書に無い")

    # 5. リンク
    for p in spec_files():
        for target in LINK.findall(p.read_text(encoding="utf-8")):
            if re.match(r"^[a-z]+:", target):
                continue
            if not (p.parent / target).resolve().exists():
                errors.append(f"リンク: {p.name} の {target} が無い")

    return errors, notes


def sort_key(n):
    k, v = n.split("-")
    return (k, int(v))


def main():
    stamp = "--stamp" in sys.argv
    errors, notes = check()
    fresh_only = all(e.startswith("鮮度:") for e in errors)
    if stamp:
        if errors and not fresh_only:
            print("点検の記録は付けない。先に直すこと:")
            for e in errors:
                if not e.startswith("鮮度:"):
                    print("  " + e)
            return 1
        lines = ["# tools/spec_check.py --stamp が書く。docs/spec/ を点検した時の元の版（git の blob）"]
        lines += [f"{blob(rel, write=True)} {rel}" for rel in WATCH]
        STAMP.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"点検の記録を付けた: {STAMP.relative_to(ROOT)}")
        return 0
    if not errors:
        print("docs/spec: 問題なし")
        return 0
    print("docs/spec: 点検が要る")
    for e in errors:
        print("  " + e)
    for n in notes:
        print(n)
    if any(e.startswith("鮮度:") for e in errors):
        print("元が変わった。docs/spec/ を点検して直し、python3 tools/spec_check.py --stamp で記録する")
    return 1


if __name__ == "__main__":
    sys.exit(main())
