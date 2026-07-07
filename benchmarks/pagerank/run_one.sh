#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$ROOT/setup_spm_env.sh" >/dev/null

VARIANT="${VARIANT:-cache}"
OUT="${OUT:-$ROOT/results/pagerank/one}"
THREADS="${THREADS:-8}"
APP_THREADS="${APP_THREADS:-8}"
NODES="${NODES:-4096}"
DEGREE="${DEGREE:-64}"
ITERS="${ITERS:-6}"
REPEAT="${REPEAT:-1}"
SNAPSHOT="${SNAPSHOT:-0}"
BASE_SET="${BASE_SET:-0}"
SPM_WAYS="${SPM_WAYS:-4}"
SVE_VL_SE="${SVE_VL_SE:-16}"

BIN="$SCRIPT_DIR/bin_gem5/pagerank_spmm_cache_mc"
WAYS=0
if [[ "$VARIANT" == "spm" ]]; then
  BIN="$SCRIPT_DIR/bin_gem5/pagerank_spmm_spm_mc"
  WAYS="$SPM_WAYS"
fi

mkdir -p "$OUT"
sve=()
for c in $(seq 0 $((THREADS - 1))); do
  sve+=("-P" "system.cpu[$c].isa[0].sve_vl_se=$SVE_VL_SE")
done

"$GEM5_SPM" --outdir="$OUT" "$GEM5_SE" --ruby \
  --num-cpus="$THREADS" --num-dirs="$THREADS" --num-l2caches="$THREADS" \
  --cpu-type=DerivO3CPU --sys-clock=2.5GHz --cpu-clock=2.5GHz \
  --mem-type=DDR4_2400_8x8 --mem-size=16GB --cacheline_size=64 \
  --l0i_size=64kB --l0d_size=64kB --l0i_assoc=4 --l0d_assoc=4 \
  --l1d_size=512kB --l1d_assoc=8 --l1d_num_spm_ways="$WAYS" \
  --l2_size=4MB --l2_assoc=16 --enable-prefetch \
  --topology=Mesh_XY --mesh-rows=2 "${sve[@]}" \
  --cmd "$BIN" \
  --options "--threads $APP_THREADS --nodes $NODES --degree $DEGREE --iters $ITERS --repeat $REPEAT --base-set $BASE_SET --snapshot $SNAPSHOT --pin 1" \
  >"$OUT/run.log" 2>&1

if grep -q 'Simulated exit code not 0' "$OUT/run.log"; then
  grep -h "pagerank_spmm\\|pthread_create\\|Simulated exit code\\|must be" "$OUT/run.log" | tail -8 || true
  exit 1
fi

grep -h "^pagerank_spmm" "$OUT/run.log" | tail -1 || true
awk '$1=="simTicks" || $1=="simSeconds"{print}' "$OUT/stats.txt" | tail -2 || true
