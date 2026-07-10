#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
unset CROSS_CXX
source "$ROOT/setup_spm_env.sh" >/dev/null
BIN="$SCRIPT_DIR/bin_gem5"; mkdir -p "$BIN"
CC="${CROSS_CXX%g++}gcc"
command -v "$CC" >/dev/null || { echo "no gcc at $CC"; exit 1; }
"$CC" -O3 -std=gnu11 -static -pthread -march=armv8.2-a+sve+fp16 \
  -mbranch-protection=none -I"$M5_INCLUDE" -S -o "$BIN/stencil_halo.s" \
  "$SCRIPT_DIR/stencil_halo_mc.c"
python3 "$SPM_COMPILER" "$BIN/stencil_halo.s" "$BIN/stencil_halo_enc.s"
"$CC" -static -pthread -march=armv8.2-a+sve+fp16 -o "$BIN/stencil_halo_mc" \
  "$BIN/stencil_halo_enc.s" "$M5OP_OBJ" -lm
ls -lh "$BIN/stencil_halo_mc"
