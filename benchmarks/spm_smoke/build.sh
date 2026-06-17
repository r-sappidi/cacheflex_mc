#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

: "${CROSS_CXX:=aarch64-none-linux-gnu-g++}"
: "${SPM_COMPILER:=$ROOT/spm_tools/spm_compiler.py}"
: "${M5_INCLUDE:=$ROOT/gem5/include}"
if [ -z "${M5OP_OBJ:-}" ]; then
    if [ -f "$ROOT/gem5/util/m5/build/arm64/out/m5op.o" ]; then
        M5OP_OBJ="$ROOT/gem5/util/m5/build/arm64/out/m5op.o"
    else
        M5OP_OBJ="$ROOT/gem5/util/m5/build/arm64/abi/arm64/m5op.o"
    fi
fi

mkdir -p "$SCRIPT_DIR/bin_gem5"

SRC="$SCRIPT_DIR/spm_multicore_smoke.cpp"
ASM="$SCRIPT_DIR/bin_gem5/spm_multicore_smoke.s"
ENC="$SCRIPT_DIR/bin_gem5/spm_multicore_smoke_enc.s"
OUT="$SCRIPT_DIR/bin_gem5/spm_multicore_smoke"

"$CROSS_CXX" \
    -O3 -std=c++17 -static -march=armv8.2-a+sve+fp16 \
    -mbranch-protection=none -DGEM5 -DGEM5_SE \
    -I"$SCRIPT_DIR" -I"$M5_INCLUDE" \
    -S -o "$ASM" "$SRC"

python3 "$SPM_COMPILER" "$ASM" "$ENC"

"$CROSS_CXX" -static -o "$OUT" "$ENC" "$M5OP_OBJ" -lm
echo "$OUT"
