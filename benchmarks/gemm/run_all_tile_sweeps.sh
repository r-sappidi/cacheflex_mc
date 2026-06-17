#!/usr/bin/env bash
# Per-variant tile-size sweep for ALL paper shapes (or a subset passed as args:
#   bash run_all_tile_sweeps.sh W4 W6 W7
# Default: W1..W7). For each shape it sweeps register-tile shape (mr x nB) x kc,
# separately for SPM and coherent, and prints the best tile per variant.
#
# Requires the generated tile binaries (benchmarks/gemm/build_tiles.sh) and a
# sourced setup_spm_env.sh. Runs are detached-friendly; launch with nohup.
#
# PC (column groups) per shape = largest of {8,4,2,1} that is <= N/nc panels.
# kc swept over {64,128,256,512,1024} filtered by K-divisibility and capacity:
#   spm: kc*(nc/16) <= 6144 SPM lines
#   blk: kc*nc*4   <= 64 KiB L0-D (deeper kc only thrashes the cache)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
G="$SCRIPT_DIR/gen"
GEM5_SPM="${GEM5_SPM:?source setup_spm_env.sh first}"
GEM5_SE="${GEM5_SE:?}"
MAXJOBS="${MAXJOBS:-6}"

# id   M    K     N
declare -A SH=(
  [W1]="128  4096  11008"
  [W2]="256  4096  4096"
  [W3]="256  2048  2048"
  [W4]="512  768   768"
  [W5]="2048 2048  2048"
  [W6]="2048 64    2048"
  [W7]="784  256   1024"
)
# tile-shape -> nc
TILES=("t8x1 64" "t4x2 128" "t6x2 128" "t8x2 128" "t4x4 256" "t5x4 256")
KC_CANDS=(64 128 256 512 1024)

IDS=("$@"); [ ${#IDS[@]} -eq 0 ] && IDS=(W1 W2 W3 W4 W5 W6 W7)

largest_pc() {  # panels -> largest of 8/4/2/1 that is <= panels
  local p=$1 d
  for d in 8 4 2 1; do [ "$p" -ge "$d" ] && { echo "$d"; return; }; done
  echo 1
}

one() {  # variant shape nc PC kc M K N id
  local variant="$1" shape="$2" nc="$3" PC="$4" kc="$5" M="$6" K="$7" N="$8" id="$9"
  local ways=0
  [ "$variant" = spm ] && ways=$(( (kc * (nc / 16) + 1023) / 1024 ))
  local out="$ROOT/results/gemm/sweep_${id}"
  local log="$out/${variant}_${shape}_kc${kc}.log"
  local sve=() c
  for c in $(seq 0 7); do sve+=("-P" "system.cpu[$c].isa[0].sve_vl_se=16"); done
  "$GEM5_SPM" --outdir="$out/d_${variant}_${shape}_kc${kc}" "$GEM5_SE" --ruby \
    --num-cpus=8 --num-dirs=8 --num-l2caches=8 --cpu-type=DerivO3CPU \
    --sys-clock=1.5GHz --cpu-clock=1.5GHz --mem-size=2GB --cacheline_size=64 \
    --l0i_size=64kB --l0d_size=64kB --l0i_assoc=4 --l0d_assoc=4 \
    --l1d_size=512kB --l1d_assoc=8 --l1d_num_spm_ways=$ways \
    --l2_size=512kB --l2_assoc=16 "${sve[@]}" --topology=Mesh_XY --mesh-rows=2 \
    --cmd "$G/gemm_${variant}_${shape}" \
    --options "--threads 8 --m $M --k $K --n $N --kc $kc --nc $nc --grid-cols $PC --warmup 1 --repeat 1 --pin 1" \
    >"$log" 2>&1
  echo "[$(date +%T)] $id $variant $shape kc$kc PC$PC ways$ways $(grep -hoE 'verify=[a-z]+' "$log" | tail -1)"
}

for id in "${IDS[@]}"; do
  read -r M K N <<<"${SH[$id]}"
  mkdir -p "$ROOT/results/gemm/sweep_${id}"
  echo "===== $id  M=$M K=$K N=$N ====="
  for spec in "${TILES[@]}"; do
    read -r shape nc <<<"$spec"
    [ $(( N % nc )) -ne 0 ] && continue           # nc must divide N
    PC=$(largest_pc $(( N / nc )))
    for variant in blk spm; do
      if [ "$variant" = spm ]; then cap=$(( 6144 / (nc / 16) )); else cap=$(( 16384 / nc )); fi
      for kc in "${KC_CANDS[@]}"; do
        [ $(( K % kc )) -ne 0 ] && continue
        [ "$kc" -gt "$K" ] && continue
        [ "$kc" -gt "$cap" ] && continue
        while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do wait -n; done
        one "$variant" "$shape" "$nc" "$PC" "$kc" "$M" "$K" "$N" "$id" &
      done
    done
  done
  wait
done
echo "[$(date +%T)] ALL SWEEPS DONE"
