#!/usr/bin/env bash
# W7 (784x256x1024), 8 cores, VL=16: sweep register-tile shape x kc separately
# for blocked and SPM, to find each one's best tile. Tile shapes come from the
# two built kernels: mc = 4x64 (4 acc), mc2 = 8x128 (16 acc). kc swept over the
# divisors of K=256 that fit each kernel's fast storage:
#   blocked nc=64  -> kc <= 256 (64 KiB L0-D);  blocked nc=128 -> kc <= 128
#   spm     nc=64  -> kc <= 1536;               spm     nc=128 -> kc <= 768
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOGROOT="$ROOT/results/gemm"
MAXJOBS="${MAXJOBS:-5}"

export NUM_DIRS=8 NUM_L2=8 L2_SIZE=512kB SVE_VL=16
export GEM5_EXTRA="--topology=Mesh_XY --mesh-rows=2"
M=784; K=256; N=1024; PC=8

# variant         nc   kc
CFGS=(
  "spm_opt_mc      64   64"
  "spm_opt_mc      64   128"
  "spm_opt_mc      64   256"
  "spm_opt_mc2     128  64"
  "spm_opt_mc2     128  128"
  "spm_opt_mc2     128  256"
  "blocked_opt_mc  64   64"
  "blocked_opt_mc  64   128"
  "blocked_opt_mc  64   256"
  "blocked_opt_mc2 128  64"
  "blocked_opt_mc2 128  128"
)

run_cfg() {
  local variant="$1" nc="$2" kc="$3"
  local label="sweepW7_${variant}_nc${nc}_kc${kc}_pc${PC}"
  local log="$LOGROOT/${label}.log"
  echo "[$(date +%T)] START $variant nc$nc kc$kc"
  GRID_COLS="$PC" KC="$kc" NC="$nc" VARIANT="$variant" LABEL="$label" \
    bash "$SCRIPT_DIR/run_mesi3_baseline.sh" 8 "$M" "$K" "$N" >"$log" 2>&1
  echo "[$(date +%T)] DONE  $variant nc$nc kc$kc rc=$? $(grep -hoE 'verify=[a-z]+' "$log" | tail -1)"
}
export -f run_cfg
export LOGROOT SCRIPT_DIR NUM_DIRS NUM_L2 L2_SIZE SVE_VL GEM5_EXTRA M K N PC

for spec in "${CFGS[@]}"; do
  read -r variant nc kc <<<"$spec"
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
  run_cfg "$variant" "$nc" "$kc" &
done
wait
echo "[$(date +%T)] ALL DONE (W7 tile sweep)"
