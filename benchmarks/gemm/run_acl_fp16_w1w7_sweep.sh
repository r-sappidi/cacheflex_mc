#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN="$SCRIPT_DIR/bin_gem5"

source "$ROOT/setup_spm_env.sh"

MAXJOBS="${MAXJOBS:-4}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/results/gemm/acl_fp16_w1w7_$(date +%Y%m%d_%H%M%S)}"
# Full W1-W7 gem5 O3/Ruby runs are already long enough that a full unmeasured
# warmup pass can dominate wall time, especially for SPM staging. Use no warmup
# unless explicitly requested by the caller.
WARMUP="${WARMUP:-0}"
REPEAT="${REPEAT:-1}"
VERIFY="${VERIFY:-8}"
THREADS="${THREADS:-8}"
MC_LIST="${MC_LIST:-128 256}"
KC_LIST="${KC_LIST:-64 128 256}"
SPM_WAYS="${SPM_WAYS:-4}"

declare -A SH=(
  [W1]="128  4096 11008"
  [W2]="256  4096 4096"
  [W3]="256  2048 2048"
  [W4]="512  768  768"
  [W5]="2048 2048 2048"
  [W6]="2048 64   2048"
  [W7]="784  256  1024"
)

grid_cols_for() {
  local M="$1" N="$2"
  local nt=384
  local panels=$(( (N + nt - 1) / nt ))
  if [ "$M" -le 256 ]; then
    local pc="$THREADS"
    if [ "$pc" -gt "$panels" ]; then
      pc=1
      local cand
      for cand in $(seq "$panels" -1 1); do
        if [ $(( THREADS % cand )) -eq 0 ]; then
          pc="$cand"
          break
        fi
      done
    fi
    echo "$pc"
  elif [ "$panels" -ge 2 ] && [ $(( THREADS % 2 )) -eq 0 ]; then
    echo 2
  else
    echo 1
  fi
}

run_one() {
  local variant="$1" id="$2" M="$3" K="$4" N="$5" mc="$6" kc="$7" pc="$8"
  local out="$RESULT_ROOT/sweep_${id}"
  local log="$out/${variant}_mc${mc}_kc${kc}.log"
  local dir="$out/d_${variant}_mc${mc}_kc${kc}"
  local ways=0
  local cmd="$BIN/gemm_acl_fp16_blk_mc"
  if [ "$variant" = spm ]; then
    ways="$SPM_WAYS"
    cmd="$BIN/gemm_acl_fp16_spm_mc"
  fi

  local sve=() c
  for c in $(seq 0 $((THREADS - 1))); do
    sve+=("-P" "system.cpu[$c].isa[0].sve_vl_se=16")
  done

	  "$GEM5_SPM" --outdir="$dir" "$GEM5_SE" --ruby \
    --num-cpus="$THREADS" --num-dirs="$THREADS" --num-l2caches="$THREADS" \
    --cpu-type=DerivO3CPU --sys-clock=1.5GHz --cpu-clock=1.5GHz \
    --mem-size=2GB --cacheline_size=64 \
    --l0i_size=64kB --l0d_size=64kB --l0i_assoc=4 --l0d_assoc=4 \
    --l1d_size=512kB --l1d_assoc=8 --l1d_num_spm_ways="$ways" \
    --l2_size=512kB --l2_assoc=16 "${sve[@]}" \
    --topology=Mesh_XY --mesh-rows=2 \
    --cmd "$cmd" \
	    --options "--threads $THREADS --m $M --k $K --n $N --mc $mc --kc $kc --grid-cols $pc --warmup $WARMUP --repeat $REPEAT --verify $VERIFY --pin 1" \
	    >"$log" 2>&1
	  local status=$?
	  if grep -q 'Simulated exit code not 0' "$log"; then
	    status=1
	  fi
	  local marker
  marker="$(grep -hoE 'verify=[A-Za-z]+' "$log" | tail -1)"
  echo "[$(date +%T)] $id $variant mc$mc kc$kc pc$pc ways$ways status=$status ${marker:-verify=missing}"
  return "$status"
}

ids=("$@")
[ ${#ids[@]} -eq 0 ] && ids=(W1 W2 W3 W4 W5 W6 W7)

mkdir -p "$RESULT_ROOT"
echo "RESULT_ROOT=$RESULT_ROOT"
echo "MAXJOBS=$MAXJOBS THREADS=$THREADS WARMUP=$WARMUP REPEAT=$REPEAT VERIFY=$VERIFY"
echo "MC_LIST=$MC_LIST"
echo "KC_LIST=$KC_LIST"

for id in "${ids[@]}"; do
  read -r M K N <<<"${SH[$id]}"
  mkdir -p "$RESULT_ROOT/sweep_${id}"
  pc="$(grid_cols_for "$M" "$N")"
  echo "===== $id M=$M K=$K N=$N grid-cols=$pc ====="
  for mc in $MC_LIST; do
    [ "$mc" -le "$M" ] || continue
    for kc in $KC_LIST; do
      [ "$kc" -le "$K" ] || continue
      [ $(( K % kc )) -eq 0 ] || continue
      [ "$kc" -le 256 ] || continue
      for variant in blk spm; do
        while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do
          wait -n
        done
        run_one "$variant" "$id" "$M" "$K" "$N" "$mc" "$kc" "$pc" &
      done
    done
  done
done

wait
echo "[$(date +%T)] ACL FP16 W1-W7 SWEEP DONE"
