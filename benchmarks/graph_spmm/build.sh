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
  -o "$BIN/graph_spmm_tile_cache_mc" \
  "$SCRIPT_DIR/graph_spmm_tile_mc.cpp" "$M5OP_OBJ" -lm

"$CROSS_CXX" "${CXXFLAGS[@]}" -DUSE_SPM -S \
  -o "$BIN/graph_spmm_tile_spm_mc.s" \
  "$SCRIPT_DIR/graph_spmm_tile_mc.cpp"

python3 "$SPM_COMPILER" \
  "$BIN/graph_spmm_tile_spm_mc.s" \
  "$BIN/graph_spmm_tile_spm_mc_enc.s"

"$CROSS_CXX" -static -pthread -march=armv8.2-a+sve+fp16 \
  -o "$BIN/graph_spmm_tile_spm_mc" \
  "$BIN/graph_spmm_tile_spm_mc_enc.s" "$M5OP_OBJ" -lm

ls -lh "$BIN/graph_spmm_tile_cache_mc" "$BIN/graph_spmm_tile_spm_mc"
