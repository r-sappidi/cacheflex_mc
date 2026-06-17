#!/usr/bin/env bash
# Summarize tile sweeps: best SPM tile, best blocked tile, and the tuned speedup
# per shape. Usage: bash harvest_sweeps.sh [W1 W2 ...]   (default: all found)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mt(){ awk '/Begin Simulation Statistics/{n++} n==2 && /^simTicks/{print $2; exit}' "$1/stats.txt" 2>/dev/null; }

IDS=("$@")
[ ${#IDS[@]} -eq 0 ] && IDS=($(ls -d "$ROOT"/results/gemm/sweep_* 2>/dev/null | sed 's#.*/sweep_##' | sort))

printf "%-4s | %-22s | %-22s | %s\n" "id" "SPM best (ticks)" "blocked best (ticks)" "speedup"
printf -- "-----+------------------------+------------------------+--------\n"
for id in "${IDS[@]}"; do
  dir="$ROOT/results/gemm/sweep_${id}"; [ -d "$dir" ] || continue
  declare -A B=()
  for var in spm blk; do
    best=""; bt=""
    for d in "$dir"/d_${var}_*; do [ -f "$d/stats.txt" ] || continue
      t=$(mt "$d"); [ -z "$t" ] && continue
      if [ -z "$bt" ] || [ "$t" -lt "$bt" ]; then bt=$t; best=$(basename "$d" | sed -E 's/d_'"$var"'_//'); fi
    done
    B[$var]="$best"; B[${var}_t]="$bt"
  done
  sp="-"
  [ -n "${B[spm_t]:-}" ] && [ -n "${B[blk_t]:-}" ] && sp=$(awk "BEGIN{printf \"%.2fx\", ${B[blk_t]}/${B[spm_t]}}")
  printf "%-4s | %-10s %-11s | %-10s %-11s | %s\n" "$id" "${B[spm]:-NA}" "${B[spm_t]:-}" "${B[blk]:-NA}" "${B[blk_t]:-}" "$sp"
done
