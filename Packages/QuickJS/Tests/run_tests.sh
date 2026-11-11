#!/bin/sh
# Builds and runs the offline idj_sandbox tests (no network, no Swift toolchain needed).
# Usage: sh Packages/QuickJS/Tests/run_tests.sh   (from the repo root)
set -e
SRC=Packages/QuickJS/Sources/CQuickJS
OUT="${TMPDIR:-/tmp}/idj_sandbox_tests"
CC="${CC:-}"
if [ -z "$CC" ]; then
  if command -v cc >/dev/null 2>&1; then CC=cc;
  elif command -v gcc >/dev/null 2>&1; then CC=gcc;
  elif command -v clang >/dev/null 2>&1; then CC=clang;
  else echo "no C compiler found (set CC)" >&2; exit 1; fi
fi
"$CC" -O1 -I "$SRC" -I "$SRC/include" \
   "$SRC/quickjs.c" "$SRC/libregexp.c" "$SRC/libunicode.c" "$SRC/cutils.c" "$SRC/xsum.c" \
   "$SRC/idj_sandbox.c" Packages/QuickJS/Tests/idj_sandbox_tests.c \
   -lm -lpthread -o "$OUT"
"$OUT"
