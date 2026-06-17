#!/usr/bin/env bash
# Run all paper GEMM shapes (W1-W7) on 8 cores for both the stock MESI
# three-level blocked baseline and the SPM variant, on a 2x4 Mesh_XY NoC
# (simple network). 8 tiles: each L0+L1 private, with a 512KB shared-L2 slice
# and one directory per tile (8 L2 slices x 512KB = 4MB total, 8 dirs).
# Bounded concurrency; per-run logs land next to the gem5 outdir.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOGROOT="$ROOT/results/gemm"
MAXJOBS="${MAXJOBS:-5}"

# Mesh / tiled-uncore config (vs the crossbar default of 1x4MB L2 + 2 dirs).
export NUM_DIRS=8
export NUM_L2=8
export L2_SIZE=512kB
export GEM5_EXTRA="--topology=Mesh_XY --mesh-rows=2"

# id  M     K     N      KC   NC
JOBS=(
  "W1 128  4096  11008  128  64"
  "W2 256  4096  4096   128  64"
  "W3 256  2048  2048   128  64"
  "W4 512  768   768    128  64"
  "W5 2048 2048  2048   128  64"
  "W6 2048 64    2048    64  64"
  "W7 784  256   1024   128  64"
)

run_one() {
  local id="$1" M="$2" K="$3" N="$4" KC="$5" NC="$6" VARIANT="$7"
  local log="$LOGROOT/${id}_${VARIANT}_mesh_8c.log"
  echo "[$(date +%T)] START $id $VARIANT mesh m$M k$K n$N kc$KC nc$NC -> $log"
  KC="$KC" NC="$NC" VARIANT="$VARIANT" LABEL="${VARIANT}_mesi3_mesh" \
    bash "$SCRIPT_DIR/run_mesi3_baseline.sh" 8 "$M" "$K" "$N" >"$log" 2>&1
  local rc=$?
  echo "[$(date +%T)] DONE  $id $VARIANT rc=$rc $(grep -hoE 'verify=[a-z]+' "$log" | tail -1)"
}
export -f run_one
export LOGROOT SCRIPT_DIR

for spec in "${JOBS[@]}"; do
  for variant in blocked spm; do
    read -r id M K N KC NC <<<"$spec"
    while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
    run_one "$id" "$M" "$K" "$N" "$KC" "$NC" "$variant" &
  done
done
wait
echo "[$(date +%T)] ALL DONE (mesh)"
