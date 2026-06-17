#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

: "${CROSS_CXX:=aarch64-none-linux-gnu-g++}"
: "${M5_INCLUDE:=$ROOT/gem5/include}"
if [ -z "${M5OP_OBJ:-}" ]; then
    if [ -f "$ROOT/gem5/util/m5/build/arm64/out/m5op.o" ]; then
        M5OP_OBJ="$ROOT/gem5/util/m5/build/arm64/out/m5op.o"
    else
        M5OP_OBJ="$ROOT/gem5/util/m5/build/arm64/abi/arm64/m5op.o"
    fi
fi

mkdir -p "$SCRIPT_DIR/bin_gem5"

SRC="$SCRIPT_DIR/flash_attention_tiled.cpp"
OUT="$SCRIPT_DIR/bin_gem5/flash_attention_tiled"

"$CROSS_CXX" \
    -O3 -std=c++17 -static -pthread -march=armv8.2-a+sve+fp16 \
    -mbranch-protection=none -DGEM5 -DGEM5_SE \
    -I"$M5_INCLUDE" \
    -o "$OUT" "$SRC" "$M5OP_OBJ" -lm

echo "$OUT"
