#!/usr/bin/env bash
# usage: run_case.sh <luamura> <lua> <test.lua> <runtime-library> [link-args...]
# Compile, link and execute a test, then compare its stdout and exit status
# against the reference Lua interpreter. LUAMURA_COMPARE=status compares
# only the exit status.
set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <luamura> <lua> <test.lua> <runtime-library> [link-args...]" >&2
    exit 2
fi

LUAMURA="$1"
LUA="$2"
LUA_FILE="$(realpath "$3")"
RUNTIME="$4"
shift 4

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

stage=compile
trap 'echo "FAIL: $stage ($LUA_FILE)" >&2' ERR
"$LUAMURA" "$LUA_FILE" "$TMP/out.ll" > "$TMP/compile.log" 2>&1 || {
    cat "$TMP/compile.log" >&2
    echo "FAIL: compile ($LUA_FILE)" >&2
    exit 1
}

stage=llc
llc "$TMP/out.ll" -filetype=obj -o "$TMP/out.o" --relocation-model=pic
stage=link
cc "$TMP/out.o" "$RUNTIME" "$@" -lm -pthread -o "$TMP/bin"

run() {
    rm -rf "$TMP/work"
    mkdir "$TMP/work"
    cp -R "$(dirname "$LUA_FILE")/." "$TMP/work/"
    local status=0
    (cd "$TMP/work" && "$@") > "$TMP/$name.out" 2> "$TMP/$name.err" || status=$?
    echo "$status" > "$TMP/$name.status"
}

stage=run
name=expected run "$LUA" "$LUA_FILE"
name=actual run "$TMP/bin"
cat "$TMP/actual.out"
cat "$TMP/actual.err" >&2

stage=compare
if [ "${LUAMURA_COMPARE:-output}" = output ]; then
    diff -u --label expected --label actual \
        "$TMP/expected.out" "$TMP/actual.out"
fi
diff -u --label "expected status" --label "actual status" \
    "$TMP/expected.status" "$TMP/actual.status" || {
    cat "$TMP/expected.err" >&2
    false
}
