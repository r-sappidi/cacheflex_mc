#!/usr/bin/env bash
# Generate + build VL=16 GEMM tile-shape variants (spm and blk) for a tile sweep.
# Shapes given as "mr:nb" args; default = the W7 candidate set.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GEN="$SCRIPT_DIR/gen"
mkdir -p "$GEN"

: "${CROSS_CXX:?source setup_spm_env.sh first}"
: "${SPM_COMPILER:?}"
: "${M5_INCLUDE:?}"
: "${M5OP_OBJ:?}"

CXXFLAGS=(-O3 -std=c++17 -static -pthread -march=armv8.2-a+sve+fp16
          -mbranch-protection=none -DGEM5 -DGEM5_SE -I"$M5_INCLUDE")

SHAPES=("$@")
[ ${#SHAPES[@]} -eq 0 ] && SHAPES=("8:1" "4:2" "6:2" "8:2" "10:2" "4:4" "5:4")

for s in "${SHAPES[@]}"; do
    mr="${s%:*}"; nb="${s#*:}"
    # blocked: compile straight through
    b="$GEN/gemm_blk_t${mr}x${nb}"
    python3 "$ROOT/spm_tools/gen_tiles.py" blk "$mr" "$nb" > "$b.cpp"
    "$CROSS_CXX" "${CXXFLAGS[@]}" -o "$b" "$b.cpp" "$M5OP_OBJ" -lm
    echo "built $b"
    # spm: through the SPM encoder pass
    p="$GEN/gemm_spm_t${mr}x${nb}"
    python3 "$ROOT/spm_tools/gen_tiles.py" spm "$mr" "$nb" > "$p.cpp"
    "$CROSS_CXX" "${CXXFLAGS[@]}" -S -o "$p.s" "$p.cpp"
    python3 "$SPM_COMPILER" "$p.s" "${p}_enc.s"
    "$CROSS_CXX" -static -pthread -o "$p" "${p}_enc.s" "$M5OP_OBJ" -lm
    echo "built $p"
done
echo "all tile variants built"
