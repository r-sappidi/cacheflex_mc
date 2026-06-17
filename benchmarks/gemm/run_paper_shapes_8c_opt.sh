#!/usr/bin/env bash
# Run all paper GEMM shapes (W1-W7) on 8 cores for the register-blocked
# OPTIMIZED kernels: gemm_blocked_opt (coherent) vs gemm_spm_opt (SPM), on a
# 2x4 Mesh_XY NoC. Mirrors run_paper_shapes_8c.sh but exercises the 4x64
# register-tile microkernels so the comparison is best-vs-best.
#
# These kernels fix nc=64 and require k % kc == 0. kc here is the K-block depth
# (larger kc -> fewer C round-trips and, for SPM, fewer staging passes, bounded
# by SPM capacity: kc*4 lines must fit ways 0..5 = 6144 lines, so kc <= 1536).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOGROOT="$ROOT/results/gemm"
MAXJOBS="${MAXJOBS:-5}"

export NUM_DIRS=8
export NUM_L2=8
export L2_SIZE=512kB
export GEM5_EXTRA="--topology=Mesh_XY --mesh-rows=2"

# Each system gets its own best K-block depth (nc fixed at 64):
#   KC_BLK: coherent panel kc*64*4B should fit the 64 KiB L0-D  -> kc <= 256
#   KC_SPM: SPM panel kc*4 lines fills ways 0..5 (<= 6144 lines) -> kc <= 1536
# Both must divide K.
# id  M     K     N      KC_BLK  KC_SPM
JOBS=(
  "W1 128  4096  11008  256  1024"
  "W2 256  4096  4096   256  1024"
  "W3 256  2048  2048   256  1024"
  "W4 512  768   768    256  768"
  "W5 2048 2048  2048   256  1024"
  "W6 2048 64    2048    64  64"
  "W7 784  256   1024   256  256"
)

run_one() {
  local id="$1" M="$2" K="$3" N="$4" KC="$5" VARIANT="$6"
  local log="$LOGROOT/${id}_${VARIANT}_mesh_8c.log"
  echo "[$(date +%T)] START $id $VARIANT mesh m$M k$K n$N kc$KC nc64 -> $log"
  KC="$KC" NC="64" VARIANT="$VARIANT" LABEL="${VARIANT}_mesi3_mesh" \
    bash "$SCRIPT_DIR/run_mesi3_baseline.sh" 8 "$M" "$K" "$N" >"$log" 2>&1
  local rc=$?
  echo "[$(date +%T)] DONE  $id $VARIANT rc=$rc $(grep -hoE 'verify=[a-z]+' "$log" | tail -1)"
}
export -f run_one
export LOGROOT SCRIPT_DIR

for spec in "${JOBS[@]}"; do
  read -r id M K N KC_BLK KC_SPM <<<"$spec"
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
  run_one "$id" "$M" "$K" "$N" "$KC_BLK" "blocked_opt" &
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
  run_one "$id" "$M" "$K" "$N" "$KC_SPM" "spm_opt" &
done
wait
echo "[$(date +%T)] ALL DONE (mesh, opt)"
