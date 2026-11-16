#!/usr/bin/env python3
"""docs/guide/ と docs/spec/ を Wiki（Wiki.js、wiki.syamei0.com）のページの木にする。

    python3 tools/wiki_sync.py OUT_DIR            # main の docs から Wiki 用の Markdown を OUT_DIR に書くだけ
    python3 tools/wiki_sync.py OUT_DIR --upload   # 書いた上で Wiki のページを作る、直す
    python3 tools/wiki_sync.py OUT_DIR --check    # 載せたページのリンクの飛び先（見出し）が在るか確かめる

中身は手で写さず、毎回 docs/ から作る。元は手元の作業中のファイルではなく、ブランチ（既定 main、--from で替える）の
中身。Wiki は利用者が入れる公開の版に合わせる（2026-11-16、ユーザ）。main 以外から作ると、各ページの頭に
「開発中の版」の付け札を出す。直すのはリポジトリの文書で、Wiki を直接直すと次の --upload で消える。
- 相対リンクは、載せる文書なら Wiki のパスへ、それ以外（例、ファーム、台帳）は GitHub の URL へ書き換える。
- 見出しへのリンク（#…）は、GitHub の付け方から Wiki.js の付け方へ写す。
- ページの頭に、どの文書のどのコミットから作ったかを書き、印（MARK）を埋める。印の無いページは上書きしない。
- API キーは環境変数 WIKI_TOKEN か ~/.config/mas-can/wiki-token（書き込みの権限が要る）。ファイルには書かない。
"""
import argparse
import base64
import json
import os
import re
import subprocess
import sys
import unicodedata
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = "https://github.com/autome-workspace/MAS_CAN_Library"
WIKI = "https://wiki.syamei0.com"
LOCALE = "ja"
BASE = "MAS/MAS-CAN"
TAGS = ["MAS-CAN"]
MARK = "<!-- mas-can-wiki:"
TOKEN_FILE = Path.home() / ".config/mas-can/wiki-token"
SRC = ROOT  # 文書を読む元。build() がブランチの中身を書き出した場所に替える
UA = {"User-Agent": "mas-can-wiki-sync/1"}  # urllib の既定の名前はサイトの前段で弾かれる


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True).stdout.strip()


# ---- 載せる文書と、Wiki のパス ----

def collect(map_page):
    """リポジトリの相対パス → Wiki のパス（locale 抜き）"""
    pages = {}
    for sub in ("guide", "spec"):
        for f in sorted((SRC / "docs" / sub).rglob("*.md")):
            rel = f.relative_to(SRC / "docs").with_suffix("")
            parts = list(rel.parts)
            if parts[-1] == "README":
                parts.pop()
            pages[str(f.relative_to(SRC))] = "/".join([BASE, *parts])
    if map_page:
        pages["docs/README.md"] = map_page
    return pages


# ---- 見出しの id ----

def heading_text(md):
    """見出しの Markdown から、表示される文字だけを取り出す"""
    parts = re.split(r"(`[^`]*`)", md)
    out = []
    for p in parts:
        if p.startswith("`"):
            out.append(p[1:-1])  # コードの中の _ や * はそのまま
            continue
        p = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", p)
        p = re.sub(r"(\*\*|\*|~~)", "", p)
        p = re.sub(r"(?<!\w)__?(\S.*?)__?(?!\w)", r"\1", p)
        out.append(p)
    return "".join(out).strip()


def github_slug(text):
    out = []
    for c in text.lower():
        cat = unicodedata.category(c)
        if c in "-_" or c == " " or cat[0] in "LMN":
            out.append("-" if c == " " else c)
    return "".join(out)


def wikijs_slug(text):
    # Wiki.js 2 の html-core（uslug ＋ 数字で始まれば h-）
    out = []
    for c in unicodedata.normalize("NFKC", text):
        cat = unicodedata.category(c)
        if cat[0] in "LMN" or c in "-_~":
            out.append(c)
        elif cat[0] == "Z" or c.isspace() or "\u3000" <= c <= "\u3002":  # 、。は区切りになる
            out.append(" ")
    s = re.sub(r"[\s\-]+", "-", "".join(out).strip()).lower()
    return "h-" + s if re.match(r"\d", s) else s


def unique(slugs, sep="-"):
    seen, out = [], []
    for s in slugs:
        if s in seen:
            i = 1
            while f"{s}{sep}{i}" in seen:
                i += 1
            s = f"{s}{sep}{i}"
        seen.append(s)
        out.append(s)
    return out


def headings(md, drop_h1):
    """(github の id, Wiki.js の id) の組の並び。drop_h1 なら先頭の h1 は Wiki に出ないので Wiki.js 側から外す"""
    texts, fence, dropped = [], False, not drop_h1
    gh_all = []
    for line in md.split("\n"):
        if line.startswith("```"):
            fence = not fence
        if fence:
            continue
        m = re.match(r"(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if not m:
            continue
        t = heading_text(m.group(2))
        gh_all.append(github_slug(t))
        if not dropped and len(m.group(1)) == 1:
            dropped = True
            texts.append(None)
            continue
        texts.append(t)
    gh = []
    counts = {}
    for s in gh_all:  # github-slugger は同じ id に -1、-2 … を付ける
        n = counts.get(s, 0)
        gh.append(s if n == 0 else f"{s}-{n}")
        counts[s] = n + 1
    wk = unique([wikijs_slug(t) for t in texts if t is not None])
    it = iter(wk)
    return {g: (next(it) if t is not None else "") for g, t in zip(gh, texts)}


# ---- 変換 ----

LINK = re.compile(r"(!?)\[((?:[^\[\]]|\[[^\]]*\])*)\]\(([^)\s]+)(\s+\"[^\"]*\")?\)")


def convert(src, md, pages, anchors, ref, unresolved):
    here = (SRC / src).parent

    def fix(m):
        bang, text, url, title = m.group(1), m.group(2), m.group(3), m.group(4) or ""
        if re.match(r"[a-z][a-z0-9+.-]*:", url) or url.startswith("//"):
            return m.group(0)
        path, _, frag = url.partition("#")
        if not path:
            target = src
        else:
            p = (here / path).resolve()
            try:
                target = str(p.relative_to(SRC))
            except ValueError:
                unresolved.append((src, url))
                return m.group(0)
            if p.is_dir():
                for cand in (p / "README.md",):
                    if str(cand.relative_to(SRC)) in pages:
                        target = str(cand.relative_to(SRC))
                        break
                else:
                    # README の無いフォルダ（guide/host/ など）は、いちばん近い上の入口へ
                    q = p
                    while q != SRC and str((q / "README.md").relative_to(SRC)) not in pages:
                        q = q.parent
                    if q != SRC and str(p.relative_to(SRC)).startswith("docs/"):
                        target = str((q / "README.md").relative_to(SRC))
                    else:
                        return f"{bang}[{text}]({REPO}/tree/{ref}/{p.relative_to(SRC)}{title})"
            elif not p.exists():
                unresolved.append((src, url))
                return m.group(0)
        if target in pages:
            dest = "" if target == src else f"/{LOCALE}/{pages[target]}"
            if frag:
                a = anchors[target].get(frag)
                if a is None:
                    unresolved.append((src, url))
                    a = frag
                frag = a
            if frag:
                return f"{bang}[{text}]({dest}#{frag}{title})"
            return f"{bang}[{text}]({dest or '#'}{title})"
        kind = "tree" if (SRC / target).is_dir() else "blob"
        return f"{bang}[{text}]({REPO}/{kind}/{ref}/{target}{'#' + frag if frag else ''}{title})"

    out, fence = [], False
    for line in md.split("\n"):
        if line.startswith("```"):
            fence = not fence
            out.append(line)
            continue
        if fence:
            out.append(line)
            continue
        # インラインのコードの中で始まるものは書き換えない（リンクの文字の中のコードは書き換える）
        code = [m.span() for m in re.finditer(r"`[^`]*`", line)]
        out.append(LINK.sub(lambda m: m.group(0) if any(a < m.start() < b for a, b in code) else fix(m), line))
    return "\n".join(out)


def snapshot(ref, dest):
    """ブランチの中身を dest に書き出す（手元の作業中の変更は入らない）"""
    global SRC
    if not git("rev-parse", "--verify", "--quiet", ref):
        sys.exit(f"{ref} というブランチ（コミット）が無い")
    subprocess.run(["rm", "-rf", str(dest)], check=True)
    dest.mkdir(parents=True)
    arc = subprocess.run(["git", "-C", str(ROOT), "archive", ref], capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", str(dest)], input=arc, check=True)
    SRC = dest.resolve()


def build(out_dir, map_page, ref):
    snapshot(ref, out_dir / "_src")
    pages = collect(map_page)
    if not pages:
        sys.exit(f"{ref} に docs/guide/ も docs/spec/ も無い。--from で別のブランチを指すか、先に {ref} に入れる")
    commit = git("rev-parse", "--short", ref)
    public = git("describe", "--tags", "--abbrev=0", "main") or "main"
    texts = {s: (SRC / s).read_text(encoding="utf-8") for s in pages}
    anchors = {s: headings(t, True) for s, t in texts.items()}
    unresolved, manifest = [], []
    for src, path in pages.items():
        md = texts[src]
        m = re.search(r"^#\s+(.+)$", md, re.M)
        title = heading_text(m.group(1)) if m else Path(src).stem
        if m:
            md = md[: m.start()] + md[m.end():]
        body = convert(src, md, pages, anchors, ref, unresolved).strip("\n")
        # 頭に出すのはこの文書を最後に変えたコミット（道具を直しただけで全ページが変わらないように）
        last = git("log", "-1", "--format=%h %cs", ref, "--", src).split() or [commit, ""]
        head = f"{MARK} {src} -->\n"
        if ref != "main":
            head += (
                f"> **開発中の版（`{ref}`）の文書です。** 今の公開版（{public}）と違う所があります。\n{{.is-warning}}\n\n"
            )
        head += (
            f"> このページは [MAS_CAN_Library]({REPO}) の [`{src}`]({REPO}/blob/{ref}/{src})（{ref}）から自動で作っています"
            f"（{last[0]}、{last[1]}）。"
            f"直すときはリポジトリの文書を直してください。ここを直しても次の更新で消えます。\n"
            "{.is-info}\n"
        )
        content = head + "\n" + body + "\n"
        f = out_dir / (path + ".md")
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(content, encoding="utf-8")
        manifest.append({"src": src, "path": path, "title": title, "file": str(f)})
    (out_dir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"{len(manifest)} ページを {out_dir} に書いた（{ref} @ {commit}）")
    for s, u in unresolved:
        print(f"  解決できないリンク: {s}: {u}")
    return manifest, unresolved


# ---- Wiki.js の API ----

def token():
    t = os.environ.get("WIKI_TOKEN") or (TOKEN_FILE.read_text().strip() if TOKEN_FILE.exists() else "")
    if not t:
        sys.exit(f"API キーが無い。環境変数 WIKI_TOKEN か {TOKEN_FILE} に置く")
    return t


def gql(query, variables=None, auth=None):
    req = urllib.request.Request(
        f"{WIKI}/graphql",
        data=json.dumps({"query": query, "variables": variables or {}}).encode(),
        headers={**UA, "Content-Type": "application/json", **({"Authorization": f"Bearer {auth}"} if auth else {})},
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        d = json.load(r)
    if d.get("errors"):
        raise RuntimeError(d["errors"])
    return d["data"]


def existing():
    d = gql("query($l:String){pages{list(locale:$l,limit:5000){id path title}}}", {"l": LOCALE})
    return {p["path"]: p for p in d["pages"]["list"]}


def upload(manifest, out_dir):
    auth = token()
    have = existing()
    backup = out_dir / "_backup"
    created = updated = same = 0
    for p in manifest:
        content = Path(p["file"]).read_text(encoding="utf-8")
        args = {
            "content": content, "description": "", "editor": "markdown", "isPublished": True,
            "isPrivate": False, "locale": LOCALE, "path": p["path"], "tags": TAGS, "title": p["title"],
        }
        old = have.get(p["path"])
        if old is None:
            r = gql(
                """mutation($content:String!,$description:String!,$editor:String!,$isPublished:Boolean!,
                $isPrivate:Boolean!,$locale:String!,$path:String!,$tags:[String]!,$title:String!){pages{create(
                content:$content,description:$description,editor:$editor,isPublished:$isPublished,
                isPrivate:$isPrivate,locale:$locale,path:$path,tags:$tags,title:$title){responseResult{succeeded message}}}}""",
                args, auth)["pages"]["create"]["responseResult"]
            created += 1
            what = "作った"
        else:
            cur = gql("query($id:Int!){pages{single(id:$id){content title}}}", {"id": old["id"]}, auth)["pages"]["single"]
            if not cur["content"].startswith(MARK):
                print(f"  飛ばした（このスクリプトが作ったページではない）: {p['path']}")
                continue
            if cur["content"] == content and cur["title"] == p["title"]:
                same += 1
                continue
            b = backup / (p["path"] + ".md")
            b.parent.mkdir(parents=True, exist_ok=True)
            b.write_text(cur["content"], encoding="utf-8")
            r = gql(
                """mutation($id:Int!,$content:String!,$description:String!,$editor:String!,$isPublished:Boolean!,
                $isPrivate:Boolean!,$locale:String!,$path:String!,$tags:[String]!,$title:String!){pages{update(
                id:$id,content:$content,description:$description,editor:$editor,isPublished:$isPublished,
                isPrivate:$isPrivate,locale:$locale,path:$path,tags:$tags,title:$title){responseResult{succeeded message}}}}""",
                {"id": old["id"], **args}, auth)["pages"]["update"]["responseResult"]
            updated += 1
            what = "直した"
        if not r["succeeded"]:
            raise RuntimeError(f"{p['path']}: {r['message']}")
        print(f"  {what}: {p['path']}")
    print(f"作った {created}、直した {updated}、変わらず {same}")
    ours = {p["path"] for p in manifest}
    stray = [x for x in have if x.startswith(BASE + "/") and x not in ours]
    for x in stray:
        print(f"  docs に無いページ（消すかは人が決める）: {x}")


def check(manifest):
    """載せたページを読み、リンクの飛び先の見出しが在るかを確かめる"""
    ids = {}
    for p in manifest:
        html = urllib.request.urlopen(urllib.request.Request(f"{WIKI}/{LOCALE}/{p['path']}", headers=UA), timeout=60).read().decode()
        m = re.search(r'toc="([^"]*)"', html)
        found = set()
        def walk(n):
            for x in n:
                found.add(x["anchor"].lstrip("#"))
                walk(x.get("children", []))
        if m:
            walk(json.loads(base64.b64decode(m.group(1))))
        found |= set(re.findall(r'\sid="([^"]+)"', html))
        ids[p["path"]] = found
    bad = 0
    for p in manifest:
        for url in re.findall(r"\]\((/" + LOCALE + r"/[^)#\s]*)?#([^)\s]+)\)", Path(p["file"]).read_text(encoding="utf-8")):
            path = url[0][len(LOCALE) + 2:] if url[0] else p["path"]
            if path in ids and url[1] not in ids[path]:
                bad += 1
                print(f"  見出しが無い: {p['path']} → {path}#{url[1]}")
    print(f"見出しへのリンク: 食い違い {bad} 件")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--upload", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--map-page", help=f"docs/README.md（docs の地図）を載せるパス。例: {BASE}。省けば載せない")
    ap.add_argument("--from", dest="ref", default="main",
                    help="文書を読むブランチ。GitHub へのリンクもこのブランチを指す。main 以外なら付け札を出す")
    a = ap.parse_args()
    manifest, unresolved = build(a.out, a.map_page, a.ref)
    if a.upload:
        if unresolved:
            sys.exit("解決できないリンクがあるので載せない")
        upload(manifest, a.out)
    if a.check:
        sys.exit(1 if check(manifest) else 0)


if __name__ == "__main__":
    main()
