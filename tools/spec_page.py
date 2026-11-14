#!/usr/bin/env python3
"""docs/spec/ の Markdown を 1 枚の HTML にまとめる（スマホで読むための artifact の元）。

    python3 tools/spec_page.py OUT.html

中身は手で写さず、毎回 docs/spec/ からそのまま作る。ページの頭に、どのブランチとコミットから作ったかを出す。
"""
import html
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "docs/spec"
REPO = "https://github.com/autome-workspace/MAS_CAN_Library"
# 読む順。README.md と uncited.md、reviewed.txt は載せない
ORDER = ["overview", "flows", "bus", "ids", "md", "servo", "settings", "board"]
LABEL = {
    "flows": "場面ごとの流れ", "overview": "全体と用語", "bus": "共通の決まり",
    "ids": "ID の一覧", "md": "MD", "servo": "サーボ", "settings": "設定", "board": "基板を作る",
}


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True).stdout.strip()


def main():
    out = Path(sys.argv[1])
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    commit = git("rev-parse", "--short", "HEAD")
    dirty = bool(git("status", "--porcelain", "--", "docs/spec"))
    docs = [{"id": n, "label": LABEL[n], "md": (SPEC / f"{n}.md").read_text(encoding="utf-8")} for n in ORDER]
    meta = {
        "branch": branch, "commit": commit, "dirty": dirty, "repo": REPO,
        "made": datetime.now().strftime("%Y-%m-%d %H:%M"),
    }
    page = TEMPLATE.replace("__DOCS__", json.dumps(docs, ensure_ascii=False).replace("</", "<\\/"))
    page = page.replace("__META__", json.dumps(meta, ensure_ascii=False))
    page = page.replace("__VERSION__", html.escape(f"{branch} @ {commit}{' ＋未コミット' if dirty else ''}"))
    out.write_text(page, encoding="utf-8")
    print(f"{out}（{branch} @ {commit}{'、未コミットあり' if dirty else ''}）")


TEMPLATE = r"""<title>MAS CAN 仕様書</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Noto+Sans+JP:wght@400;600;700&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
/* 読む文書: 細い本文の列、上に版と目次。目次は横に流せる帯 */
:root {
  --bg: #f7f8fa; --surface: #ffffff; --fg: #1b2130; --muted: #5d6677;
  --line: #dde1e8; --accent: #1f6f78; --accent-soft: #e3f1f2; --code-bg: #eef1f5;
  --body: "Noto Sans JP", system-ui, -apple-system, "Hiragino Sans", sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, "SFMono-Regular", Menlo, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #12161d; --surface: #181d26; --fg: #e3e7ee; --muted: #9aa3b2;
  --line: #2a3140; --accent: #6cc3cb; --accent-soft: #1d3236; --code-bg: #1f2531; color-scheme: dark } }
:root[data-theme="dark"] {
  --bg: #12161d; --surface: #181d26; --fg: #e3e7ee; --muted: #9aa3b2;
  --line: #2a3140; --accent: #6cc3cb; --accent-soft: #1d3236; --code-bg: #1f2531; color-scheme: dark }
body { background: var(--bg); color: var(--fg); font-family: var(--body); font-size: 16px; line-height: 1.8; }
body::before { content: ""; position: fixed; inset: 0 0 auto; height: env(safe-area-inset-top, 0px); z-index: 10; background: var(--surface); }
header { position: sticky; top: env(safe-area-inset-top, 0px); z-index: 5; background: var(--surface); border-bottom: 1px solid var(--line);
  padding-block: 10px 0; padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); }
.top { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; max-width: 820px; margin: 0 auto; }
.top strong { font-size: 15px; letter-spacing: .02em; }
.ver { font-family: var(--mono); font-size: 12px; color: var(--muted); }
nav { max-width: 820px; margin: 8px auto 0; overflow-x: auto; scrollbar-width: none; }
nav::-webkit-scrollbar { display: none; }
nav ol { display: flex; gap: 6px; list-style: none; margin: 0; padding: 0 0 10px; white-space: nowrap; }
nav a { display: block; padding: 4px 12px; border-radius: 999px; border: 1px solid var(--line); color: var(--muted);
  text-decoration: none; font-size: 13px; }
nav a[aria-current="page"] { background: var(--accent-soft); border-color: var(--accent); color: var(--fg); font-weight: 600; }
nav a:focus-visible, main a:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
main { max-width: 820px; margin: 0 auto; padding-block: 8px 48px;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); }
main h1 { font-size: 26px; line-height: 1.4; margin: 24px 0 16px; text-wrap: balance; }
main h2 { font-size: 20px; line-height: 1.5; margin: 40px 0 12px; padding-top: 12px; border-top: 1px solid var(--line); text-wrap: balance; }
main h3 { font-size: 17px; margin: 28px 0 8px; }
main p, main li { max-width: 40em; }
main a { color: var(--accent); }
main code { font-family: var(--mono); font-size: .88em; background: var(--code-bg); padding: 1px 5px; border-radius: 4px; }
main pre { background: var(--code-bg); padding: 12px 14px; border-radius: 6px; overflow-x: auto; line-height: 1.55; }
main pre code { background: none; padding: 0; font-size: 13px; }
.tw { overflow-x: auto; margin: 12px 0; }
main table { border-collapse: collapse; font-size: 14px; line-height: 1.6; min-width: 100%; }
main th, main td { border: 1px solid var(--line); padding: 6px 10px; text-align: left; vertical-align: top; }
main th { background: var(--code-bg); font-weight: 600; }
main td { font-variant-numeric: tabular-nums; }
.pager { display: flex; justify-content: space-between; gap: 12px; margin-top: 48px; padding-top: 16px; border-top: 1px solid var(--line); flex-wrap: wrap; }
.pager a { text-decoration: none; }
.note { font-size: 13px; color: var(--muted); }
</style>

<header>
  <div class="top"><strong>MAS CAN 仕様書</strong><span class="ver">__VERSION__</span></div>
  <nav aria-label="文書"><ol id="nav"></ol></nav>
</header>
<main id="doc"><p class="note">読み込んでいます…</p></main>

<script src="https://cdnjs.cloudflare.com/ajax/libs/marked/12.0.2/marked.min.js"></script>
<script>
const DOCS = __DOCS__;
const META = __META__;
const ids = DOCS.map(d => d.id);
const navEl = document.getElementById("nav");
const docEl = document.getElementById("doc");

navEl.innerHTML = DOCS.map(d => `<li><a href="#doc-${d.id}" data-id="${d.id}">${d.label}</a></li>`).join("");

// リポジトリの中の相対パスを、GitHub の同じブランチの URL にする
function repoUrl(path) {
  const parts = ("docs/spec/" + path).split("/");
  const out = [];
  for (const p of parts) { if (p === "..") out.pop(); else if (p && p !== ".") out.push(p); }
  return `${META.repo}/blob/${META.branch}/${out.join("/")}`;
}

function render(id) {
  const d = DOCS.find(x => x.id === id) || DOCS[0];
  docEl.innerHTML = marked.parse(d.md);
  docEl.querySelectorAll("table").forEach(t => {
    const w = document.createElement("div"); w.className = "tw"; t.replaceWith(w); w.appendChild(t);
  });
  docEl.querySelectorAll("a[href]").forEach(a => {
    const href = a.getAttribute("href");
    if (/^[a-z]+:/i.test(href) || href.startsWith("#")) return;
    const m = href.match(/^([\w-]+)\.md(#.*)?$/);
    if (m && ids.includes(m[1])) { a.setAttribute("href", "#doc-" + m[1]); return; }
    a.setAttribute("href", repoUrl(href.split("#")[0])); a.target = "_blank"; a.rel = "noopener";
  });
  const i = ids.indexOf(d.id);
  const prev = i > 0 ? `<a href="#doc-${DOCS[i-1].id}">← ${DOCS[i-1].label}</a>` : "<span></span>";
  const next = i < DOCS.length - 1 ? `<a href="#doc-${DOCS[i+1].id}">${DOCS[i+1].label} →</a>` : "<span></span>";
  docEl.insertAdjacentHTML("beforeend",
    `<div class="pager">${prev}${next}</div><p class="note">docs/spec/ から ${META.made} に作成（${META.branch} @ ${META.commit}${META.dirty ? "、未コミットの変更を含む" : ""}）。正本はリポジトリ。</p>`);
  navEl.querySelectorAll("a").forEach(a => {
    if (a.dataset.id === d.id) { a.setAttribute("aria-current", "page"); a.scrollIntoView({block: "nearest", inline: "center"}); }
    else a.removeAttribute("aria-current");
  });
  window.scrollTo(0, 0);
}

function route() {
  const m = location.hash.match(/^#doc-([\w-]+)$/);
  render(m ? m[1] : "overview");
}
window.addEventListener("hashchange", route);
if (window.marked) route();
else docEl.innerHTML = '<p class="note">表示のための部品を読み込めませんでした。再読み込みしてください。</p>';
</script>
"""

if __name__ == "__main__":
    main()
