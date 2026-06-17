#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

: "${GEM5_SPM:=$ROOT/gem5/build/ARM_MESI_Three_Level_SPM/gem5.opt}"
: "${GEM5_SE:=$ROOT/gem5/configs/deprecated/example/se.py}"

CORES="${1:-8}"
M="${2:-512}"
K="${3:-768}"
N="${4:-768}"
KC="${KC:-128}"
NC="${NC:-64}"
REPEAT="${REPEAT:-1}"
VARIANT="${VARIANT:-blocked}"   # blocked | spm
LABEL="${LABEL:-${VARIANT}_mesi3}"
BIN="$SCRIPT_DIR/bin_gem5/gemm_${VARIANT}"
OUTDIR="$ROOT/results/gemm/${LABEL}_${CORES}c_m${M}k${K}n${N}"

# Private-L1 ways statically reserved for SPM (Option B). The B panel claims
# kc*(nc/16) SPM lines into 1024-set ways starting at way0, so it needs
# ceil(lines/1024) ways. Only meaningful for the SPM variant; the blocked
# baseline keeps all 8 ways coherent.
case "$VARIANT" in
    spm*)
        lines=$(( KC * (NC / 16) ))
        SPM_WAYS="${SPM_WAYS:-$(( (lines + 1023) / 1024 ))}"
        ;;
    *)
        SPM_WAYS="${SPM_WAYS:-0}"
        ;;
esac

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run benchmarks/gemm/build.sh first" >&2
    exit 1
fi

SVE_VL="${SVE_VL:-4}"   # SVE vector length in 128-bit granules (4=512b, 16=2048b)
sve_args=()
for ((core = 0; core < CORES; core++)); do
    sve_args+=("-P" "system.cpu[$core].isa[0].sve_vl_se=$SVE_VL")
done

mkdir -p "$OUTDIR"
"$GEM5_SPM" \
    --outdir="$OUTDIR" \
    "$GEM5_SE" \
    --ruby \
    --num-cpus="$CORES" \
    --num-dirs="${NUM_DIRS:-2}" \
    --num-l2caches="${NUM_L2:-1}" \
    --cpu-type=DerivO3CPU \
    --sys-clock=1.5GHz \
    --cpu-clock=1.5GHz \
    --mem-size=2GB \
    --cacheline_size=64 \
    --l0i_size=64kB \
    --l0d_size=64kB \
    --l0i_assoc=4 \
    --l0d_assoc=4 \
    --l1d_size=512kB \
    --l1d_assoc=8 \
    --l1d_num_spm_ways="$SPM_WAYS" \
    --l2_size="${L2_SIZE:-4MB}" \
    --l2_assoc=16 \
    "${sve_args[@]}" \
    ${GEM5_EXTRA:-} \
    --cmd "$BIN" \
    --options "--threads $CORES --m $M --k $K --n $N --kc $KC --nc $NC --warmup 1 --repeat $REPEAT --pin 1 ${GRID_COLS:+--grid-cols $GRID_COLS}"

echo "$OUTDIR"
