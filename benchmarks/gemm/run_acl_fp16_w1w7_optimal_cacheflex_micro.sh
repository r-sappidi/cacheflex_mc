#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN="$SCRIPT_DIR/bin_gem5"

source "$ROOT/setup_spm_env.sh"

MAXJOBS="${MAXJOBS:-1}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/results/gemm/acl_fp16_w1w7_optimal_cacheflex_micro_$(date +%Y%m%d_%H%M%S)}"
WARMUP="${WARMUP:-0}"
REPEAT="${REPEAT:-1}"
VERIFY="${VERIFY:-16}"
THREADS="${THREADS:-8}"
SPM_WAYS="${SPM_WAYS:-4}"
ENABLE_PREFETCH="${ENABLE_PREFETCH:-1}"

# cacheflex_micro Cortex-style Ruby mapping:
#   Ruby L0  -> private L1I/L1D
#   Ruby L1D -> private unified L2, with optional CacheFlex SPM way reservation
#   Ruby L2  -> banked LLC behind the private L2s
#
# This matches the Cortex-A710-inspired paper table used for the prior
# single-core evaluation: 64KB private L1s, 512KB private L2/SPM, 4MB LLC bank,
# 2.5GHz clocks, 5-wide frontend/rename, 4-wide commit, 8-wide
# dispatch/issue/writeback, and the same ROB/RF/LSQ sizing used below.
CPU_CLOCK="${CPU_CLOCK:-2.5GHz}"
SYS_CLOCK="${SYS_CLOCK:-2.5GHz}"
MEM_SIZE="${MEM_SIZE:-8GB}"
L1I_SIZE="${L1I_SIZE:-64kB}"
L1D_SIZE="${L1D_SIZE:-64kB}"
L1I_ASSOC="${L1I_ASSOC:-4}"
L1D_ASSOC="${L1D_ASSOC:-4}"
PRIVATE_L2_SIZE="${PRIVATE_L2_SIZE:-512kB}"
PRIVATE_L2_ASSOC="${PRIVATE_L2_ASSOC:-8}"
LLC_BANK_SIZE="${LLC_BANK_SIZE:-4MB}"
LLC_ASSOC="${LLC_ASSOC:-16}"

# Preserve the old 8x3VL W1-W7 comparison path. These binaries/tiles were
# selected for VL16; changing to VL1/128b is a separate kernel path.
SVE_VL_SE="${SVE_VL_SE:-16}"

declare -A SH=(
  [W1]="128  4096 11008"
  [W2]="256  4096 4096"
  [W3]="256  2048 2048"
  [W4]="512  768  768"
  [W5]="2048 2048 2048"
  [W6]="2048 64   2048"
  [W7]="784  256  1024"
)

# Previously selected best-to-best 8x3VL points from the preserved old results.
declare -A OPT=(
  [W1_blk]="128 256 8"
  [W1_spm]="128 128 8"
  [W2_blk]="96 64 8"
  [W2_spm]="96 128 8"
  [W3_blk]="96 128 4"
  [W3_spm]="96 256 4"
  [W4_blk]="96 64 2"
  [W4_spm]="96 256 2"
  [W5_blk]="96 128 2"
  [W5_spm]="96 256 2"
  [W6_blk]="96 64 2"
  [W6_spm]="96 64 2"
  [W7_blk]="96 128 2"
  [W7_spm]="96 128 2"
)

run_one() {
  local variant="$1" id="$2" M="$3" K="$4" N="$5" mc="$6" kc="$7" pc="$8"
  local out="$RESULT_ROOT/$id/${variant}_mc${mc}_kc${kc}_pc${pc}"
  local log="$out/run.log"
  local ways=0
  local cmd="$BIN/gemm_acl_fp16_blk_mc"
  if [ "$variant" = spm ]; then
    ways="$SPM_WAYS"
    cmd="$BIN/gemm_acl_fp16_spm_mc"
  fi

  mkdir -p "$out"
  local sve=() c prefetch=()
  for c in $(seq 0 $((THREADS - 1))); do
    sve+=("-P" "system.cpu[$c].isa[0].sve_vl_se=$SVE_VL_SE")
    sve+=("-P" "system.cpu[$c].fetchWidth=5")
    sve+=("-P" "system.cpu[$c].decodeWidth=5")
    sve+=("-P" "system.cpu[$c].renameWidth=5")
    sve+=("-P" "system.cpu[$c].dispatchWidth=8")
    sve+=("-P" "system.cpu[$c].issueWidth=8")
    sve+=("-P" "system.cpu[$c].wbWidth=8")
    sve+=("-P" "system.cpu[$c].commitWidth=4")
    sve+=("-P" "system.cpu[$c].numROBEntries=128")
    sve+=("-P" "system.cpu[$c].instQueues[0].numEntries=80")
    sve+=("-P" "system.cpu[$c].LQEntries=32")
    sve+=("-P" "system.cpu[$c].SQEntries=48")
    sve+=("-P" "system.cpu[$c].SPMLQEntries=32")
    sve+=("-P" "system.cpu[$c].SPMSQEntries=48")
    sve+=("-P" "system.cpu[$c].numPhysIntRegs=128")
    sve+=("-P" "system.cpu[$c].numPhysFloatRegs=192")
    sve+=("-P" "system.cpu[$c].numPhysVecRegs=192")
    sve+=("-P" "system.cpu[$c].numPhysVecPredRegs=64")
  done
  if [ "$ENABLE_PREFETCH" -ne 0 ]; then
    prefetch+=(--enable-prefetch)
  fi

  "$GEM5_SPM" --outdir="$out" "$GEM5_SE" --ruby \
    --num-cpus="$THREADS" --num-dirs="$THREADS" --num-l2caches="$THREADS" \
    --cpu-type=DerivO3CPU --sys-clock="$SYS_CLOCK" --cpu-clock="$CPU_CLOCK" \
    --mem-size="$MEM_SIZE" --cacheline_size=64 \
    --l0i_size="$L1I_SIZE" --l0d_size="$L1D_SIZE" --l0i_assoc="$L1I_ASSOC" --l0d_assoc="$L1D_ASSOC" \
    --l1d_size="$PRIVATE_L2_SIZE" --l1d_assoc="$PRIVATE_L2_ASSOC" --l1d_num_spm_ways="$ways" \
    --l2_size="$LLC_BANK_SIZE" --l2_assoc="$LLC_ASSOC" "${prefetch[@]}" "${sve[@]}" \
    --topology=Mesh_XY --mesh-rows=2 \
    --cmd "$cmd" \
    --options "--threads $THREADS --m $M --k $K --n $N --mc $mc --kc $kc --grid-cols $pc --warmup $WARMUP --repeat $REPEAT --verify $VERIFY --pin 1" \
    >"$log" 2>&1

  local rc=$?
  if grep -q 'Simulated exit code not 0' "$log"; then
    rc=1
  fi
  local marker ticks seconds gflops status
  marker="$(grep -hoE 'verify=[A-Za-z]+' "$log" | tail -1 || true)"
  ticks="$(awk '$1=="simTicks"{print $2}' "$out/stats.txt" 2>/dev/null | tail -1)"
  seconds="$(awk '$1=="simSeconds"{print $2}' "$out/stats.txt" 2>/dev/null | tail -1)"
  if [ "$rc" -eq 0 ] && [[ "$marker" == "verify=pass" ]] && [ -n "$seconds" ]; then
    status=pass
    gflops="$(awk -v m="$M" -v k="$K" -v n="$N" -v r="$REPEAT" -v s="$seconds" 'BEGIN{printf "%.6f", (2*m*k*n*r)/(s*1e9)}')"
  else
    status=FAIL
    gflops=nan
  fi
  {
    echo "shape,variant,m,k,n,threads,mc,kc,grid_cols,spm_ways,status,simTicks,simSeconds,gflops,log,stats"
    echo "$id,$variant,$M,$K,$N,$THREADS,$mc,$kc,$pc,$ways,$status,${ticks:-nan},${seconds:-nan},$gflops,$log,$out/stats.txt"
  } >"$out/result.csv"
  echo "[$(date +%T)] $id $variant mc$mc kc$kc pc$pc ways$ways status=$status gflops=$gflops ${marker:-verify=missing}"
}

ids=("$@")
[ ${#ids[@]} -eq 0 ] && ids=(W1 W2 W3 W4 W5 W6 W7)

mkdir -p "$RESULT_ROOT"
{
  echo "benchmark=ACL_style_vs_CacheFlex_SPM_8x3VL_optimal_cacheflex_micro_cortex_config"
  echo "config_mapping=Ruby L0=64KB private L1, Ruby L1=512KB private L2/SPM, Ruby L2=4MB LLC bank"
  echo "threads=$THREADS warmup=$WARMUP repeat=$REPEAT verify=$VERIFY spm_ways=$SPM_WAYS maxjobs=$MAXJOBS enable_prefetch=$ENABLE_PREFETCH"
  echo "cpu=DerivO3CPU_cortex_a710_inspired sys_clock=$SYS_CLOCK cpu_clock=$CPU_CLOCK mem=$MEM_SIZE line=64B"
  echo "core_params=fetch5 decode5 rename5 dispatch8 issue8 wb8 commit4 ROB128 IQ80 LQ32 SQ48 SPMLQ32 SPMSQ48 IntRF128 FPRF192 VecRF192 PredRF64"
  echo "sve_vl_se=$SVE_VL_SE note=old_8x3VL_path_uses_SPM_VL=16"
  echo "l0i=$L1I_SIZE/$L1I_ASSOC l0d=$L1D_SIZE/$L1D_ASSOC private_l2=$PRIVATE_L2_SIZE/$PRIVATE_L2_ASSOC spm_reserved_ways=$SPM_WAYS llc_bank=$LLC_BANK_SIZE/$LLC_ASSOC aggregate_llc=$((THREADS))x$LLC_BANK_SIZE"
  echo "source_summary=$ROOT/results/gemm/acl_fp16_w1w7_final_summary/best_to_best_w1_w7_vl16_8core.csv"
  sha256sum "$BIN/gemm_acl_fp16_blk_mc" "$BIN/gemm_acl_fp16_spm_mc" 2>/dev/null || true
} >"$RESULT_ROOT/manifest.txt"

echo "RESULT_ROOT=$RESULT_ROOT"
for id in "${ids[@]}"; do
  read -r M K N <<<"${SH[$id]}"
  echo "===== $id M=$M K=$K N=$N ====="
  for variant in blk spm; do
    read -r mc kc pc <<<"${OPT[${id}_${variant}]}"
    while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do
      wait -n
    done
    run_one "$variant" "$id" "$M" "$K" "$N" "$mc" "$kc" "$pc" &
  done
done
wait

python3 - "$RESULT_ROOT" <<'PY'
import csv, glob, math, os, sys
root = sys.argv[1]
rows = []
for path in sorted(glob.glob(os.path.join(root, "W*", "*", "result.csv"))):
    with open(path) as f:
        rows.extend(csv.DictReader(f))
if not rows:
    raise SystemExit("no result rows")
with open(os.path.join(root, "summary.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=rows[0].keys())
    w.writeheader()
    w.writerows(rows)
by_shape = {}
for r in rows:
    by_shape.setdefault(r["shape"], {})[r["variant"]] = r
out_rows = []
for shape in sorted(by_shape):
    b = by_shape[shape].get("blk")
    s = by_shape[shape].get("spm")
    if not b or not s:
        continue
    bg = float(b["gflops"]) if b["gflops"] != "nan" else math.nan
    sg = float(s["gflops"]) if s["gflops"] != "nan" else math.nan
    speedup = sg / bg if bg and math.isfinite(bg) and math.isfinite(sg) else math.nan
    out_rows.append({
        "shape": shape,
        "baseline_gflops": f"{bg:.6f}" if math.isfinite(bg) else "nan",
        "spm_gflops": f"{sg:.6f}" if math.isfinite(sg) else "nan",
        "speedup": f"{speedup:.6f}" if math.isfinite(speedup) else "nan",
        "baseline_tile": f"mc{b['mc']}_kc{b['kc']}_grid{b['grid_cols']}",
        "spm_tile": f"mc{s['mc']}_kc{s['kc']}_grid{s['grid_cols']}",
        "baseline_status": b["status"],
        "spm_status": s["status"],
        "baseline_stats": b["stats"],
        "spm_stats": s["stats"],
    })
with open(os.path.join(root, "best_to_best.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=out_rows[0].keys())
    w.writeheader()
    w.writerows(out_rows)
vals = [float(r["speedup"]) for r in out_rows if r["speedup"] != "nan"]
geo = math.prod(vals) ** (1 / len(vals)) if vals else math.nan
arith = sum(vals) / len(vals) if vals else math.nan
with open(os.path.join(root, "README.md"), "w") as f:
    f.write("# ACL-style vs CacheFlex SPM 8x3VL optimal rerun with cacheflex_micro Cortex config\n\n")
    f.write(f"- Geomean speedup: {geo:.6f}x\n")
    f.write(f"- Arithmetic mean speedup: {arith:.6f}x\n")
    f.write("- Source: best_to_best.csv and summary.csv\n")
print(f"summary={os.path.join(root, 'best_to_best.csv')}")
print(f"geomean={geo:.6f}")
PY
echo "[$(date +%T)] CACHEFLEX_MICRO_CORTEX_CONFIG OPTIMAL RERUN DONE"
