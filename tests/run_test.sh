#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
case "${1:-all}" in
    -h|--help) echo "usage: $0 [all|internal|lua-test]"; exit 0 ;;
    all) label=e2e ;;
    internal|lua-test) label=$1 ;;
    *) echo "usage: $0 [all|internal|lua-test]" >&2; exit 2 ;;
esac
if [ "$#" -gt 1 ]; then
    echo "usage: $0 [all|internal|lua-test]" >&2
    exit 2
fi

cmake -S "$ROOT" -B "$ROOT/build"
cmake --build "$ROOT/build" --parallel
ctest --test-dir "$ROOT/build" -L "^${label}$" --output-on-failure --no-tests=error
