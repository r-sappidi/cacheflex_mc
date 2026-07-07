#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

unset CROSS_CXX
source "$ROOT/setup_spm_env.sh" >/dev/null

BIN="$SCRIPT_DIR/bin_gem5"
mkdir -p "$BIN"

CXXFLAGS=(-O3 -std=c++17 -static -pthread -march=armv8.2-a+sve+fp16
          -mbranch-protection=none -DGEM5 -DGEM5_SE -I"$M5_INCLUDE")

"$CROSS_CXX" "${CXXFLAGS[@]}" \
  -o "$BIN/pagerank_spmm_cache_mc" \
  "$SCRIPT_DIR/pagerank_spmm_mc.cpp" "$M5OP_OBJ" -lm

"$CROSS_CXX" "${CXXFLAGS[@]}" -DUSE_SPM -S \
  -o "$BIN/pagerank_spmm_spm_mc.s" \
  "$SCRIPT_DIR/pagerank_spmm_mc.cpp"

python3 "$SPM_COMPILER" \
  "$BIN/pagerank_spmm_spm_mc.s" \
  "$BIN/pagerank_spmm_spm_mc_enc.s"

"$CROSS_CXX" -static -pthread -march=armv8.2-a+sve+fp16 \
  -o "$BIN/pagerank_spmm_spm_mc" \
  "$BIN/pagerank_spmm_spm_mc_enc.s" "$M5OP_OBJ" -lm

ls -lh "$BIN/pagerank_spmm_cache_mc" "$BIN/pagerank_spmm_spm_mc"
