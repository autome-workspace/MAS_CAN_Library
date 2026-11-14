#!/bin/sh
# Claude Code の PostToolUse フック。台帳や core のヘッダを変えたら、docs/spec の点検を促す。止めはしない。
f=$(jq -r '.tool_input.file_path // empty')
case "$f" in
*/docs/internal/decisions/*.md | */lib/mas/include/mas/can/node.h | */lib/mas/include/mas/can/core/* | \
    */lib/mas/src/can/node.c | */lib/mas/src/can/host.cpp) ;;
*) exit 0 ;;
esac
root=$(dirname "$0")/..
if out=$(python3 "$root/tools/spec_check.py" 2>&1); then exit 0; fi
n=$(printf '%s\n' "$out" | grep -c '^  ')
head=$(printf '%s\n' "$out" | head -15)
msg=$(printf '%s を変えた。外部向けの docs/spec/ を点検し、直したら python3 tools/spec_check.py --stamp で記録する（指摘 %s 件。全部は python3 tools/spec_check.py）。\n%s' "$f" "$n" "$head")
jq -n --arg c "$msg" '{hookSpecificOutput: {hookEventName: "PostToolUse", additionalContext: $c}}'
