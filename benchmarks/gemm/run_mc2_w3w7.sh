#!/usr/bin/env bash
# W3 and W7, 8 cores, VL=16, with the PROPERLY-TILED kernel (mr=8, nc=128 -> 16
# accumulators, compute-bound) vs the coherent baseline at the same tile. Tests
# whether the load-bound nc=64 mc kernel was inflating the W3/W7 speedups.
# blocked kc capped at 128 (kc*128*4 <= 64 KiB L0-D); SPM kc up to ways 0..5
# (kc*8 lines <= 6144).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOGROOT="$ROOT/results/gemm"
MAXJOBS="${MAXJOBS:-4}"

export NUM_DIRS=8 NUM_L2=8 L2_SIZE=512kB SVE_VL=16
export GEM5_EXTRA="--topology=Mesh_XY --mesh-rows=2"

# id  M    K     N     PC  KC_SPM  KC_BLK
JOBS=(
  "W3 256  2048  2048  8   512  128"
  "W7 784  256   1024  8   256  128"
)

run_one() {
  local id="$1" M="$2" K="$3" N="$4" PC="$5" kc="$6" variant="$7" label="$8"
  local log="$LOGROOT/${id}_${variant}_vl16_8c.log"
  echo "[$(date +%T)] START $id $variant PC$PC kc$kc -> $log"
  GRID_COLS="$PC" KC="$kc" NC="128" VARIANT="$variant" LABEL="$label" \
    bash "$SCRIPT_DIR/run_mesi3_baseline.sh" 8 "$M" "$K" "$N" >"$log" 2>&1
  echo "[$(date +%T)] DONE  $id $variant rc=$? $(grep -hoE 'grid=[0-9]+x[0-9]+|mr=[0-9]+|verify=[a-z]+' "$log" | tr '\n' ' ')"
}
export -f run_one
export LOGROOT SCRIPT_DIR NUM_DIRS NUM_L2 L2_SIZE SVE_VL GEM5_EXTRA

for spec in "${JOBS[@]}"; do
  read -r id M K N PC KC_SPM KC_BLK <<<"$spec"
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
  run_one "$id" "$M" "$K" "$N" "$PC" "$KC_BLK" blocked_opt_mc2 "${id}_blocked_opt_mc2_vl16" &
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
  run_one "$id" "$M" "$K" "$N" "$PC" "$KC_SPM" spm_opt_mc2 "${id}_spm_opt_mc2_vl16" &
done
wait
echo "[$(date +%T)] ALL DONE (mc2 w3 w7)"
