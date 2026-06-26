#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

unset CROSS_CXX
source "$ROOT/setup_spm_env.sh"

BIN="$SCRIPT_DIR/bin_gem5"
mkdir -p "$BIN"
SPM_VL="${SPM_VL:-16}"
case "$SPM_VL" in
  1|2|4|8|16) ;;
  *) echo "bad SPM_VL=$SPM_VL; expected one of 1,2,4,8,16" >&2; exit 2 ;;
esac

"$CROSS_CXX" -O3 -std=c++17 -static -pthread \
  -march=armv8.2-a+sve+fp16 -mbranch-protection=none \
  -DGEM5 -DGEM5_SE -I"$M5_INCLUDE" \
  -I"$ROOT/../cacheflex_micro/kernels/llama_bench" \
  -o "$BIN/gemm_acl_fp16_blk_mc" \
  "$SCRIPT_DIR/gemm_acl_fp16_blk_mc.cpp" "$M5OP_OBJ" -lm

"$CROSS_CXX" -O3 -std=c++17 -static -pthread \
  -march=armv8.2-a+sve+fp16 -mbranch-protection=none \
  -DGEM5 -DGEM5_SE -DVL_"$SPM_VL" -I"$M5_INCLUDE" \
  -I"$ROOT/../cacheflex_micro/kernels/llama_bench_spm" \
  -S -o "$BIN/gemm_acl_fp16_spm_mc.s" \
  "$SCRIPT_DIR/gemm_acl_fp16_spm_mc.cpp"

python3 "$SPM_COMPILER" \
  "$BIN/gemm_acl_fp16_spm_mc.s" \
  "$BIN/gemm_acl_fp16_spm_mc_enc.s"

"$CROSS_CXX" -static -pthread \
  -o "$BIN/gemm_acl_fp16_spm_mc" \
  "$BIN/gemm_acl_fp16_spm_mc_enc.s" "$M5OP_OBJ" -lm

echo "Built SPM path with -DVL_$SPM_VL"
ls -lh "$BIN/gemm_acl_fp16_blk_mc" "$BIN/gemm_acl_fp16_spm_mc"
