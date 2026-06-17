#!/usr/bin/env bash
# Full tile-size sweep for W7 (784x256x1024), 8 cores, VL=16, 2x4-or-2D mesh.
# Sweeps register-tile shape (mr x nB) x kc, separately for SPM and coherent,
# to find each one's optimal tile. PC (column groups) is set by nc: nc<=128 ->
# PC=8 (8/16 panels), nc=256 -> PC=4 (only 4 panels). SPM ways = ceil(panel
# lines / 1024). Logs one line per config to results/gemm/sweepW7/.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
G="$SCRIPT_DIR/gen"
OUT="$ROOT/results/gemm/sweepW7"
mkdir -p "$OUT"
GEM5_SPM="${GEM5_SPM:?source setup_spm_env.sh}"
GEM5_SE="${GEM5_SE:?}"
M=784; K=256; N=1024
MAXJOBS="${MAXJOBS:-6}"

# shape  nc   PC
SHAPES=("t8x1 64 8" "t4x2 128 8" "t6x2 128 8" "t8x2 128 8" "t4x4 256 4" "t5x4 256 4")
KCS=(64 128 256)

one() {
  local variant="$1" shape="$2" nc="$3" PC="$4" kc="$5"
  local ways=0
  [ "$variant" = spm ] && ways=$(( (kc * (nc / 16) + 1023) / 1024 ))
  local bin="$G/gemm_${variant}_${shape}"
  local log="$OUT/${variant}_${shape}_kc${kc}.log"
  local sve=()
  local c
  for c in 0 1 2 3 4 5 6 7; do sve+=("-P" "system.cpu[$c].isa[0].sve_vl_se=16"); done
  "$GEM5_SPM" --outdir="$OUT/d_${variant}_${shape}_kc${kc}" "$GEM5_SE" --ruby \
    --num-cpus=8 --num-dirs=8 --num-l2caches=8 --cpu-type=DerivO3CPU \
    --sys-clock=1.5GHz --cpu-clock=1.5GHz --mem-size=2GB --cacheline_size=64 \
    --l0i_size=64kB --l0d_size=64kB --l0i_assoc=4 --l0d_assoc=4 \
    --l1d_size=512kB --l1d_assoc=8 --l1d_num_spm_ways=$ways \
    --l2_size=512kB --l2_assoc=16 "${sve[@]}" \
    --topology=Mesh_XY --mesh-rows=2 --cmd "$bin" \
    --options "--threads 8 --m $M --k $K --n $N --kc $kc --nc $nc --grid-cols $PC --warmup 1 --repeat 1 --pin 1" \
    >"$log" 2>&1
  echo "[$(date +%T)] done $variant $shape kc$kc ways$ways $(grep -hoE 'verify=[a-z]+' "$log" | tail -1)"
}

for spec in "${SHAPES[@]}"; do
  read -r shape nc PC <<<"$spec"
  for kc in "${KCS[@]}"; do
    for variant in blk spm; do
      while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
      one "$variant" "$shape" "$nc" "$PC" "$kc" &
    done
  done
done
wait
echo "[$(date +%T)] W7 FULL TILE SWEEP DONE"
