# Retained-source SPMCP update — 2026-10-01

Current runner: `scripts/run_rumur_refresh_v6.py`; live results:
`results/rumur_refresh_20261001/verification_v6/status.json`.
The v5 two-core/five-operation run found an unexpected drain-liveness failure:
source-release PUTs emptied the LLC sharer list while retaining SS. Evidence is
preserved under v5/diagnosis_zero_sharer. It is not a passing bound.

SPMCP now takes a snapshot without releasing existing coherent ownership.
L0 hits carry a full-line data message to the private controller, leaving the L0
copy intact; private-controller hits install from existing TBE storage and retain
coherent state. Misses still use GETS_SILENT. A prior L0 eviction may require the
existing recall fallback; that fallback retains private coherent ownership.
Lazy migration preserves coherent source data; capture into the TBE precedes
slot allocation. SPMWB invalidation and memory acknowledgment are unchanged.

The model includes one pending L0 snapshot per outstanding CPU copy, permitting
coherence activity between capture and installation. It does not model physical
link timing or every finite-network queue. New invariants forbid source-release
PUTs under RETAIN_SOURCE=1 and require a live copy operation for a pending snapshot.
Setting RETAIN_SOURCE=0 re-enables the old semantics as an expected-liveness
negative control at two cores/five operations. All prior14 cases are repeated,
plus this15th control. Existing snapshots and evidence remain unchanged.

Publication requires software to avoid conflicting cross-domain accesses;
retained coherent copies still participate in ordinary coherence. Read-only
snapshots may be shared. This is not a general concurrent cross-domain atomicity
claim. Formal acceptance alone is not a full simulator runtime validation.

---

# Current-protocol verification refresh — 2026-10-01

These are bounded abstractions, not equivalence proofs of SLICC/C++ or exhaustive
verification of the complete eight-core machine. Earlier models and run logs are
preserved in the parent directory; the parent README describes historical behavior.

## Models and coverage

* `private_spm.m`: two/three-core, single-coherent-line message-level model with
  one CPU operation outstanding per core. Local coherent SPMCP hits relinquish
  their source using PUTS/PUTE/PUTM and await Put_Ack; forwards during that wait
  preserve the snapshot in a TBE. Misses use non-registering silent snapshots.
  Retry NACKs are consumed into the original transaction, then reissued when
  request capacity is available. Writeback invalidates coherent copies before
  installing the SPM value. No sticky wb_race exemption remains. Migration allows
  transient occupants and keeps the two-free-coherent-way capacity contract.
  Retained silent fills use ordinary directory GETS to record LLC ownership;
  non-retained fills leave no untracked LLC copy. Both retention modes are tested.
  PUT-wait states cannot accept new CPU operations: after home processes a PUT,
  they can retain data until its acknowledgment without representing a readable
  coherent copy. SWMR covers CPU-accessible copies; a separate invariant checks
  that waiting sources stay busy with their original copy operation.
* `network_progress.m`: a **directed wait-for dependency model**, only nine
  reachable states with both repairs enabled. It checks the two specified cycles
  and negative controls. It is NOT a topology, packet-routing, FIFO implementation,
  VC allocator, fairness, or full-network deadlock proof. Its repairs correspond
  to response-path NACK/TBE retry and separation of existing request VC classes.
* `l3_ordering.m`: two workers/slots, initialization-update-finalization sequences,
  captured/in-flight/sink/complete command states, early transport acknowledgment,
  four sink records, coherent-destination invalidation/acknowledgment, pending
  accounting and release/handoff protection. Removing source RAW protection,
  sink order, or pending-release protection must yield named counterexamples.
  Arithmetic is symbolic integer state. No FP rounding, port timing, full LSQ
  implementation, arbitrary slot aliases, repeated ownership epochs, or OS
  isolation proof is implied.

## Execution and acceptance

`python3 scripts/run_rumur_refresh_v5.py` from the repository root is the current
runner. Prefer inspecting its existing status before starting another attempt:
`results/rumur_refresh_20261001/verification_v5/status.json`.
The top-level `verification/rumur/run_cacheflex_spm.sh` checks for a live refresh
before invoking it; it never defaults to all host threads.

The checker is compiled with two threads and a two-CPU affinity subset of the
existing experiment budget. One case runs at a time. A reservation in the shared
broker and a per-process address-space limit constrain generation, compilation,
and checking. Larger bounds wait for their complete reservations. Timeout/OOM,
compile failure, or any unexpected assertion is NOT a verification pass.

Positive acceptance requires a successful exit AND `No error found.`. Negative
acceptance requires an unsuccessful exit, the named expected invariant/assertion
or liveness violation, and a corresponding trace. Merely crashing does not pass.
Independent cases continue after findings so one failure does not hide coverage.

The 14-case plan includes two-core bounds of 2/3/4/5 CPU operations, three cores
at 3 operations, the alternate retention mode, and negative controls. These are
operation bounds, not cycle or state-depth cutoffs. The private model retains
unordered message pools and a folded L0 FIFO; finite-network claims must remain
separate. Existing inherited pool overflows are model-bound findings, not evidence
of real hardware queue overflow.

## Provenance and current results

Each attempt saves model copies, source/tool hashes, parameterized input,
generated C, checker executable, command lines, logs, and result JSON. Rumur was
built locally with one compiler job; binary and upstream commit are retained in
`results/rumur_refresh_20261001/tools`.

`verification_v1` is superseded: it still retained the historical local-copy
semantics. `verification_v2` exposed two abstraction errors (retained LLC directory
ownership and classification of PUT-wait states); its failures are preserved.
`verification_v3` failed generation on a type-name typo; no check ran.
`verification_v4` exposed a stale ghost assertion attributing a delayed normal
Unblock registration to a later silent fetch; the check now compares metadata
before/after the silent-request transition.
`verification_v5` repeats the checks after those corrections. Read its results;
do not substitute the historical README's clean-state counts for new acceptance.

See SOURCE_MAP.md for implementation anchors. There is no automatic SLICC-to-
Murphi translation or demonstrated refinement relation. The formal results and
the gem5 directed/math tests provide different evidence and must be reported so.
