#!/usr/bin/env bash
# Focused W1..W7 multicore GEMM sweep.
#
# This keeps the paper workload set (W1-W7) but avoids the exhaustive
# tile/kc grid.  Each shape gets two strong register-tile candidates for both
# coherent-cache and CacheFlex-SPM variants:
#   - t8x2, nc=128: good row reuse and the most stable generated kernel.
#   - t4x4, nc=256: more B-column reuse with the same accumulator count.
#
# kc is fixed to 64 for W6 because K=64; otherwise it uses kc=64 for t8x2 and
# kc=128 for t4x4, subject to divisibility and capacity checks.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
G="$SCRIPT_DIR/gen"
GEM5_SPM="${GEM5_SPM:?source setup_spm_env.sh first}"
GEM5_SE="${GEM5_SE:?}"
MAXJOBS="${MAXJOBS:-4}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/results/gemm/focused_$(date +%Y%m%d_%H%M%S)}"
WARMUP="${WARMUP:-1}"
REPEAT="${REPEAT:-1}"

declare -A SH=(
  [W1]="128  4096  11008"
  [W2]="256  4096  4096"
  [W3]="256  2048  2048"
  [W4]="512  768   768"
  [W5]="2048 2048  2048"
  [W6]="2048 64    2048"
  [W7]="784  256   1024"
)

IDS=("$@")
[ ${#IDS[@]} -eq 0 ] && IDS=(W1 W2 W3 W4 W5 W6 W7)

largest_pc() {
  local p=$1 d
  for d in 8 4 2 1; do
    [ "$p" -ge "$d" ] && { echo "$d"; return; }
  done
  echo 1
}

kc_for() {
  local id="$1" shape="$2"
  if [ "$id" = W6 ]; then
    echo 64
  elif [ "$shape" = t8x2 ]; then
    echo 64
  else
    echo 128
  fi
}

one() {
  local variant="$1" shape="$2" nc="$3" PC="$4" kc="$5" M="$6" K="$7" N="$8" id="$9"
  local ways=0
  [ "$variant" = spm ] && ways=$(( (kc * (nc / 16) + 1023) / 1024 ))
  local out="$RESULT_ROOT/sweep_${id}"
  local log="$out/${variant}_${shape}_kc${kc}.log"
  local sve=() c
  for c in $(seq 0 7); do
    sve+=("-P" "system.cpu[$c].isa[0].sve_vl_se=16")
  done
  "$GEM5_SPM" --outdir="$out/d_${variant}_${shape}_kc${kc}" "$GEM5_SE" --ruby \
    --num-cpus=8 --num-dirs=8 --num-l2caches=8 --cpu-type=DerivO3CPU \
    --sys-clock=1.5GHz --cpu-clock=1.5GHz --mem-size=2GB --cacheline_size=64 \
    --l0i_size=64kB --l0d_size=64kB --l0i_assoc=4 --l0d_assoc=4 \
    --l1d_size=512kB --l1d_assoc=8 --l1d_num_spm_ways=$ways \
    --l2_size=512kB --l2_assoc=16 "${sve[@]}" --topology=Mesh_XY --mesh-rows=2 \
    --cmd "$G/gemm_${variant}_${shape}" \
    --options "--threads 8 --m $M --k $K --n $N --kc $kc --nc $nc --grid-cols $PC --warmup $WARMUP --repeat $REPEAT --pin 1" \
    >"$log" 2>&1
  echo "[$(date +%T)] $id $variant $shape kc$kc PC$PC ways$ways $(grep -hoE 'verify=[a-z]+' "$log" | tail -1)"
}

mkdir -p "$RESULT_ROOT"
echo "RESULT_ROOT=$RESULT_ROOT"
echo "MAXJOBS=$MAXJOBS WARMUP=$WARMUP REPEAT=$REPEAT"

for id in "${IDS[@]}"; do
  read -r M K N <<<"${SH[$id]}"
  mkdir -p "$RESULT_ROOT/sweep_${id}"
  echo "===== $id  M=$M K=$K N=$N ====="
  for spec in "t8x2 128" "t4x4 256"; do
    read -r shape nc <<<"$spec"
    [ $(( N % nc )) -ne 0 ] && continue
    kc="$(kc_for "$id" "$shape")"
    [ $(( K % kc )) -ne 0 ] && continue
    PC="$(largest_pc $(( N / nc )))"
    for variant in blk spm; do
      if [ "$variant" = spm ]; then
        cap=$(( 6144 / (nc / 16) ))
      else
        cap=$(( 131072 / nc ))
      fi
      [ "$kc" -gt "$cap" ] && continue
      while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do
        wait -n
      done
      one "$variant" "$shape" "$nc" "$PC" "$kc" "$M" "$K" "$N" "$id" &
    done
  done
done

wait
echo "[$(date +%T)] FOCUSED SWEEPS DONE"
