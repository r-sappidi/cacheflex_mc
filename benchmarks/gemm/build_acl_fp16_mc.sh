#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

unset CROSS_CXX
source "$ROOT/setup_spm_env.sh"

BIN="$SCRIPT_DIR/bin_gem5"
mkdir -p "$BIN"

"$CROSS_CXX" -O3 -std=c++17 -static -pthread \
  -march=armv8.2-a+sve+fp16 -mbranch-protection=none \
  -DGEM5 -DGEM5_SE -I"$M5_INCLUDE" \
  -I"$ROOT/../cacheflex_micro/kernels/llama_bench" \
  -o "$BIN/gemm_acl_fp16_blk_mc" \
  "$SCRIPT_DIR/gemm_acl_fp16_blk_mc.cpp" "$M5OP_OBJ" -lm

"$CROSS_CXX" -O3 -std=c++17 -static -pthread \
  -march=armv8.2-a+sve+fp16 -mbranch-protection=none \
  -DGEM5 -DGEM5_SE -DVL_16 -I"$M5_INCLUDE" \
  -I"$ROOT/../cacheflex_micro/kernels/llama_bench_spm" \
  -S -o "$BIN/gemm_acl_fp16_spm_mc.s" \
  "$SCRIPT_DIR/gemm_acl_fp16_spm_mc.cpp"

python3 "$SPM_COMPILER" \
  "$BIN/gemm_acl_fp16_spm_mc.s" \
  "$BIN/gemm_acl_fp16_spm_mc_enc.s"

"$CROSS_CXX" -static -pthread \
  -o "$BIN/gemm_acl_fp16_spm_mc" \
  "$BIN/gemm_acl_fp16_spm_mc_enc.s" "$M5OP_OBJ" -lm

ls -lh "$BIN/gemm_acl_fp16_blk_mc" "$BIN/gemm_acl_fp16_spm_mc"
