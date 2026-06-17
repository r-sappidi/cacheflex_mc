#!/usr/bin/env bash
# All paper shapes W1-W7, 8 cores, VL=16 (2048-bit), 2D PRxPC grid tiling, on
# the 2x4 Mesh_XY NoC. Runs both the SPM (gemm_spm_opt_mc) and coherent
# (gemm_blocked_opt_mc) kernels best-vs-best.
#
# For every paper shape the operand staged in SPM is B (split along N) -- none of
# W1-W7 is the large-M/small-N case where A-staging would win. What varies per
# shape is the grid (PC column groups, PR = 8/PC) and the K-block depth, chosen
# from operand sizes:
#   - huge B / small A (W1-W3): pure N-split (PC=8, PR=1) -> B disjoint 1x, A in LLC
#   - balanced W4 / both-huge W5: 2D 2x4 (PC=4, PR=2)
#   - tiny operands W6/W7: pure N-split (PC=8); SPM ~neutral (all cache-resident)
# KC_SPM keeps the kc x nc panel within SPM ways 0..5 (kc*4 <= 6144 lines).
# KC_BLK keeps the coherent kc x nc panel hot in the 64 KiB L0-D (kc <= 256).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOGROOT="$ROOT/results/gemm"
MAXJOBS="${MAXJOBS:-4}"

export NUM_DIRS=8
export NUM_L2=8
export L2_SIZE=512kB
export SVE_VL=16
export GEM5_EXTRA="--topology=Mesh_XY --mesh-rows=2"

# id  M     K     N      PC  KC_SPM  KC_BLK
JOBS=(
  "W1 128  4096  11008  8   1024  256"
  "W2 256  4096  4096   8   1024  256"
  "W3 256  2048  2048   8   1024  256"
  "W4 512  768   768    4   768   256"
  "W5 2048 2048  2048   4   1024  256"
  "W6 2048 64    2048   8   64    64"
  "W7 784  256   1024   8   256   256"
)

run_one() {
  local id="$1" M="$2" K="$3" N="$4" PC="$5" kc="$6" variant="$7" label="$8"
  local log="$LOGROOT/${id}_${variant}_vl16_8c.log"
  echo "[$(date +%T)] START $id $variant vl16 8c PC$PC kc$kc -> $log"
  GRID_COLS="$PC" KC="$kc" NC="64" VARIANT="$variant" LABEL="$label" \
    bash "$SCRIPT_DIR/run_mesi3_baseline.sh" 8 "$M" "$K" "$N" >"$log" 2>&1
  echo "[$(date +%T)] DONE  $id $variant rc=$? $(grep -hoE 'grid=[0-9]+x[0-9]+|verify=[a-z]+' "$log" | tr '\n' ' ')"
}
export -f run_one
export LOGROOT SCRIPT_DIR NUM_DIRS NUM_L2 L2_SIZE SVE_VL GEM5_EXTRA

for spec in "${JOBS[@]}"; do
  read -r id M K N PC KC_SPM KC_BLK <<<"$spec"
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
  run_one "$id" "$M" "$K" "$N" "$PC" "$KC_BLK" blocked_opt_mc "${id}_blocked_opt_mc_vl16" &
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
  run_one "$id" "$M" "$K" "$N" "$PC" "$KC_SPM" spm_opt_mc "${id}_spm_opt_mc_vl16" &
done
wait
echo "[$(date +%T)] ALL DONE (all shapes mc vl16 8c)"
