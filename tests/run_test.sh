#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
case "${1:-all}" in
    -h|--help) echo "usage: $0 [all|internal|lua-test]"; exit 0 ;;
    all) target=check ;;
    internal|lua-test) target=check-$1 ;;
    *) echo "usage: $0 [all|internal|lua-test]" >&2; exit 2 ;;
esac
if [ "$#" -gt 1 ]; then
    echo "usage: $0 [all|internal|lua-test]" >&2
    exit 2
fi

make -C "$ROOT/src/luamura" -k -j"$(nproc)" "$target"
