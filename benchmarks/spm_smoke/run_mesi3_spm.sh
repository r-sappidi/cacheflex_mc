#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

: "${GEM5_SPM:=$ROOT/gem5/build/ARM_MESI_Three_Level_SPM/gem5.opt}"
: "${GEM5_SE:=$ROOT/gem5/configs/deprecated/example/se.py}"

CORES="${1:-2}"
ITERS="${2:-100}"
SETS_PER_CORE="${3:-32}"
BASE_SET="${4:-128}"
BIN="$SCRIPT_DIR/bin_gem5/spm_multicore_smoke"
OUTDIR="$ROOT/results/spm_smoke/${CORES}c_${ITERS}i_${SETS_PER_CORE}s_base${BASE_SET}"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run benchmarks/spm_smoke/build.sh first" >&2
    exit 1
fi

cmds=()
opts=()
for ((core = 0; core < CORES; core++)); do
    cmds+=("$BIN")
    opts+=("$core $CORES $ITERS $SETS_PER_CORE $BASE_SET")
done

cmd_joined="$(IFS=';'; echo "${cmds[*]}")"
opt_joined="$(IFS=';'; echo "${opts[*]}")"
sve_args=()
for ((core = 0; core < CORES; core++)); do
    sve_args+=("-P" "system.cpu[$core].isa[0].sve_vl_se=4")
done

mkdir -p "$OUTDIR"
"$GEM5_SPM" \
    --outdir="$OUTDIR" \
    "$GEM5_SE" \
    --ruby \
    --num-cpus="$CORES" \
    --cpu-type=DerivO3CPU \
    --sys-clock=1.5GHz \
    --cpu-clock=1.5GHz \
    --mem-size=2GB \
    --cacheline_size=64 \
    --l1i_size=64kB \
    --l1d_size=256kB \
    --l2_size=512kB \
    --l1i_assoc=4 \
    --l1d_assoc=16 \
    --l2_assoc=8 \
    "${sve_args[@]}" \
    --cmd "$cmd_joined" \
    --options "$opt_joined"

echo "$OUTDIR"
