#!/usr/bin/env bash
#
# Verify the CacheFlex SPM multicore protocol with Rumur.  There is a SINGLE
# unified model, cacheflex_spm_mc.m, covering both:
#   * the coherent-source / snapshot / writeback CONTROL plane (SWMR, data
#     freshness, silent-fetch non-registration, deadlock/panic findings F1..F8),
#     multi-core (NCORE=2 and 3), and
#   * the physical L1 set/way PLACEMENT plane: SPMCP_install / SPMCP_fetch claim
#     ways, lazily MIGRATE a displaced coherent line to a free way, and honour
#     the ">= MIN_FREE available non-SPM ways" software contract -- exercised
#     *together* with live cross-core coherence traffic on the migrated line.
#
# See cacheflex_spm_mc.m's header for scope, abstractions, and the findings
# F1..F9.  The search is bounded-complete (a STEPS fuel counter caps CPU ops
# so the state space is finite and fully explorable); deadlock detection is off
# and the liveness "all cores can drain" property is the stuck-state detector.
#
# Requires the `rumur` binary on PATH (https://github.com/Smattr/rumur).
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

RUMUR="${RUMUR:-rumur}"
CC="${CC:-cc}"
THREADS="${THREADS:-$(nproc)}"
MODEL=cacheflex_spm_mc.m

# run the model with overrides for any of the config constants
run() {  # name [KEY=VAL ...]
  local name="$1"; shift
  local m="build/${name}.m"
  cp "${MODEL}" "${m}"
  local kv key val
  for kv in "$@"; do
    key="${kv%%=*}"; val="${kv#*=}"
    sed -i -E "s/^( *${key}:[[:space:]]*)[0-9]+;/\1${val};/" "${m}"
  done
  "${RUMUR}" --deadlock-detection off --output "build/${name}.c" "${m}"
  "${CC}" -std=c11 -O3 -pthread -mcx16 "build/${name}.c" -o "build/${name}" -latomic
  echo "=== ${name}: $* ==="
  "./build/${name}" --threads "${THREADS}"
}

# 1) Unified verification: coherence + placement/migration together, clean.
run u_n2_s5 NCORE=2 STEPS=5 ASSUME_FIXES=1 MIN_FREE=2
run u_n3_s3 NCORE=3 STEPS=3 ASSUME_FIXES=1 MIN_FREE=2
run u_n2_s4 NCORE=2 STEPS=4 ASSUME_FIXES=1 MIN_FREE=2

# 2) Placement-contract necessity: the >=MIN_FREE contract keeps the lazy
#    migration panic-free.  MIN_FREE=1 is the strict minimum; MIN_FREE=0 reaches
#    the no-free-way migration panic.  (Both expected to differ, so || true.)
echo "=== contract necessity (expected: MIN_FREE=1 clean, MIN_FREE=0 fails) ==="
run u_mf1 NCORE=2 STEPS=4 MIN_FREE=1 || true
run u_mf0 NCORE=2 STEPS=4 MIN_FREE=0 || true

# 3) Finding F9: install displacing a coherence-TRANSIENT line hits gem5's
#    IsStableCoherent assert.  INSTALL_STABLE_ONLY=0 drops the software contract
#    that install targets never alias a transient line's way.  Expected to fail.
echo "=== F9 demo (INSTALL_STABLE_ONLY=0, expected to fail) ==="
run u_f9 NCORE=2 STEPS=4 INSTALL_STABLE_ONLY=0 || true

# 4) Control-plane findings F1..F8: with the assumed fixes DISABLED the model
#    aborts at the first reachable SLICC gap (F1).  Fix-forward one at a time to
#    walk F1..F8; each is documented inline.  Expected to fail.
echo "=== control-plane findings demo (ASSUME_FIXES=0, expected to fail) ==="
run u_findings NCORE=3 STEPS=4 ASSUME_FIXES=0 || true
