#!/usr/bin/env bash
# W4 (m512 k768 n768), 8 cores, VL=16 (2048-bit), 2D PRxPC grid tiling.
# Runs the new multicore kernels (gemm_*_opt_mc) best-vs-best:
#   SPM     kc=768  (panel fills SPM ways 0..5)
#   blocked kc=256  (panel kept hot in L0-D)
# on the 2x4 Mesh_XY NoC, same fabric as the prior 8c sweep. Logs to results.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOGROOT="$ROOT/results/gemm"

export NUM_DIRS=8
export NUM_L2=8
export L2_SIZE=512kB
export SVE_VL=16
export GEM5_EXTRA="--topology=Mesh_XY --mesh-rows=2"

M=512; K=768; N=768

run_one() {
  local variant="$1" kc="$2" label="$3"
  local log="$LOGROOT/W4_${variant}_vl16_8c.log"
  echo "[$(date +%T)] START $variant vl16 8c kc$kc -> $log"
  KC="$kc" NC="64" VARIANT="$variant" LABEL="$label" \
    bash "$SCRIPT_DIR/run_mesi3_baseline.sh" 8 "$M" "$K" "$N" >"$log" 2>&1
  echo "[$(date +%T)] DONE  $variant rc=$? $(grep -hoE 'verify=[a-z]+' "$log" | tail -1)"
}

run_one blocked_opt_mc 256 blocked_opt_mc_mesi3_mesh_vl16 &
run_one spm_opt_mc     768 spm_opt_mc_mesi3_mesh_vl16 &
wait
echo "[$(date +%T)] ALL DONE (W4 mc vl16 8c)"
