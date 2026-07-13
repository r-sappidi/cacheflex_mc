#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
unset CROSS_CXX
source "$ROOT/setup_spm_env.sh" >/dev/null
BIN="$SCRIPT_DIR/bin_gem5"; mkdir -p "$BIN"
CXXFLAGS=(-O2 -std=c++17 -static -pthread -march=armv8.2-a+sve+fp16
          -mbranch-protection=none)
"$CROSS_CXX" "${CXXFLAGS[@]}" -S -o "$BIN/spm_store_smoke.s" \
  "$SCRIPT_DIR/spm_store_smoke.cpp"
python3 "$SPM_COMPILER" "$BIN/spm_store_smoke.s" "$BIN/spm_store_smoke_enc.s"
"$CROSS_CXX" -static -pthread -march=armv8.2-a+sve+fp16 \
  -o "$BIN/spm_store_smoke" "$BIN/spm_store_smoke_enc.s" -lm
ls -lh "$BIN/spm_store_smoke"
