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

CXXFLAGS=(-O3 -std=c++17 -static -pthread -march=armv8.2-a+sve+fp16
          -mbranch-protection=none -DGEM5 -DGEM5_SE -I"$M5_INCLUDE")

# Coherent (stock-MESI) variants compile straight through the cross compiler.
for src in gemm_blocked gemm_blocked_opt gemm_blocked_opt_mc gemm_blocked_opt_mc2; do
    OUT="$SCRIPT_DIR/bin_gem5/$src"
    "$CROSS_CXX" "${CXXFLAGS[@]}" \
        -o "$OUT" "$SCRIPT_DIR/$src.cpp" "$M5OP_OBJ" -lm
    echo "$OUT"
done

# The SPM variants go through spm_compiler.py to encode the CacheFlex
# SPM pseudo-instructions emitted as inline asm.
for src in gemm_spm gemm_spm_opt gemm_spm_opt_mc gemm_spm_opt_mc2; do
    ASM="$SCRIPT_DIR/bin_gem5/$src.s"
    ENC="$SCRIPT_DIR/bin_gem5/${src}_enc.s"
    OUT="$SCRIPT_DIR/bin_gem5/$src"
    "$CROSS_CXX" "${CXXFLAGS[@]}" -S -o "$ASM" "$SCRIPT_DIR/$src.cpp"
    python3 "$SPM_COMPILER" "$ASM" "$ENC"
    "$CROSS_CXX" -static -pthread -o "$OUT" "$ENC" "$M5OP_OBJ" -lm
    echo "$OUT"
done
