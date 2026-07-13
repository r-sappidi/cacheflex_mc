#!/usr/bin/env bash
# usage: MODE=cache|pull [SPM_WAYS=..] [ROWS=..] [COLS=..] [ITERS=..] [SPINBAR=0|1] ./run_stencil_halo_8core.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$ROOT/setup_spm_env.sh" >/dev/null
MODE="${MODE:-cache}"; ROWS="${ROWS:-64}"; COLS="${COLS:-1024}"; ITERS="${ITERS:-48}"
SPINBAR="${SPINBAR:-0}"
if [[ "$MODE" == "cache" ]]; then WAYS="${SPM_WAYS:-0}"; else WAYS="${SPM_WAYS:-4}"; fi
BIN="$SCRIPT_DIR/bin_gem5/stencil_halo_mc"
[[ -x "$BIN" ]] || "$SCRIPT_DIR/build_stencil_halo.sh"
OUT="${OUT_ROOT:-$ROOT/results/stencil_halo}/${MODE}_r${ROWS}c${COLS}i${ITERS}"
mkdir -p "$OUT"
"$GEM5_SPM" --outdir="$OUT" "$GEM5_SE" --ruby \
  --num-cpus=8 --num-dirs=8 --num-l2caches=8 \
  --cpu-type=DerivO3CPU --sys-clock=2.5GHz --cpu-clock=2.5GHz \
  --mem-type=DDR4_2400_8x8 --mem-size=16GB --cacheline_size=64 \
  --l0i_size=64kB --l0d_size=64kB --l0i_assoc=4 --l0d_assoc=4 \
  --l1d_size=512kB --l1d_assoc=8 --l1d_num_spm_ways="$WAYS" \
  --l2_size=4MB --l2_assoc=16 --enable-prefetch --topology=Mesh_XY --mesh-rows=2 \
  $(for c in 0 1 2 3 4 5 6 7; do echo "-P system.cpu[$c].isa[0].sve_vl_se=16"; done) \
  --cmd "$BIN" \
  --options "--threads=8 --rows=$ROWS --cols=$COLS --iters=$ITERS --pin=1 --mode=$MODE --spin-barrier=$SPINBAR" \
  2>&1 | tee "$OUT/gem5.log" | tail -3
awk '$1=="simTicks"{print "roi_ticks="$2; exit}' "$OUT/stats.txt"
