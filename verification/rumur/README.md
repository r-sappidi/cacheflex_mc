# CacheFlex SPM multicore — Rumur model

`cacheflex_spm_mc.m` is a message-level formal model of the **current**
copy/snapshot SPM extension to gem5 `MESI_Three_Level`, written for the
[Rumur](https://github.com/Smattr/rumur) explicit-state model checker. It
replaces the older table-level models (`cacheflex_spm_2core.m`,
`cacheflex_spm_tables_complete.m`, `gen_table_model.py`), which described the
legacy PUT-based *release* semantics and are gone.

## What it models

Sources of truth: the three SLICC controllers
`gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-{L1cache,L2cache,dir}.sm` and
the harvested tables `spm_coherency_{L0,L1,dir}.csv`.

- `NCORE` private-L1 controllers (the SPM-holding level), each with an
  abstracted core-side L0 and one private SPM slot
- one shared L2/LLC directory bank, the memory directory, and main memory
- one coherent line address; per-core SPM slot addresses
- **copy/snapshot `SPMCP_fetch`**: the source line stays coherent (SS/EE/MM);
  local S/E/M sources recall L0 for freshness first; an absent source issues
  `GETS_SILENT`, served by L2 SS/M, forwarded silently to an MT owner, or
  filled from memory — the requester is **never** registered as sharer/owner
- `SPMCP_install` / `SPMLD` / `SPMST` / `SPMWB_store` (through L2+dir to
  memory) / `SPM_release`
- coherent LD/ST/UPGRADE, the L0↔L1 recall/writeback races, L1/L2 replacement,
  directory `Fetch`/`Fetch_Silent`/`SPM_Writeback`/`MEM_Inv`

Message networks are bounded **unordered** pools (Ruby reorders same-address
messages via `stall_and_wait`/`recycle`); the ordered L0↔L1 buffers are a FIFO.
A `(state,event)` pair with no SLICC transition and no stall is a gem5 **panic**;
the model raises an `error` for those, so the checker decides the reachability
of every suspected undefined transition.

Deliberately excluded (unreachable or out of scope in the current build): the
legacy release path (`PUTS/PUTM/PUTE`, L1 `*X_A`, dir `SPM_PUT_WB`), flush, HTM,
prefetch, LL/SC, instruction fetch, DMA, multiple L2 banks.

### Unified placement plane (set/way + lazy migration)

The **same** model also carries a physical L1 set of `WAYS` ways per core. The
coherent line and the SPM slots share that set, so `SPMCP_install` /
`SPMCP_fetch` can land on the way holding the coherent line and must **lazily
migrate** it to a free way (pure local placement — the directory tracks lines
at core granularity and is way-agnostic, so migration is **coherence-neutral**)
under the "keep ≥ `MIN_FREE` non-SPM (available) ways" software contract. This
is exercised *together with live cross-core coherence traffic on the migrated
line*, which is the whole point of unifying it with the multi-core model rather
than checking placement on a one-core model in isolation.

Two obligations only the unified model can check:

- **F9** — install may only displace a **stable** coherent occupant (gem5
  `allocateSPMSlot` asserts `IsStableCoherent`). Displacing a line mid-
  coherence-transaction panics; `INSTALL_STABLE_ONLY` models the software
  contract that install targets never alias a transient line's way.
- **no-free-way** — because the coherent line itself occupies one non-SPM way,
  the migration's free-way math is `WAYS − spm_ways − line_resident`; the
  contract must reserve enough non-SPM ways. Verified boundary: `MIN_FREE=0`
  reaches the no-free-way panic, `MIN_FREE=1` is the strict minimum, and
  `MIN_FREE=2` (the CacheFlex contract) is clean with one way of headroom.

## Checked properties

Coherence (SWMR + data-value freshness via ghost `latest`, silent-fetch
non-registration, L2/dir metadata consistency), SPM slot isolation, the
placement invariants (set never oversubscribed, contract keeps `MIN_FREE`
non-SPM ways, migration never hits the no-free-way panic), and a **liveness**
"all cores can drain" property that detects protocol deadlocks — all checked
together in one model.

## Running

```sh
./run_cacheflex_spm.sh          # needs `rumur` on PATH; RUMUR=/path overrides
```

The search is **bounded-but-complete**: a `STEPS` fuel counter caps total CPU
operations, making the reachable state space finite and fully explorable (new
coherence activity only ever originates at a CPU op). Structural deadlock
detection is off (a fuel-exhausted quiescent state has no successors by
design); the liveness property is the stuck-state detector.

Exhaustive-within-bound, all coherence **and** placement properties pass
together **with the assumed fixes on** (`MIN_FREE=2`):

| config | states | result |
|--------|-------:|--------|
| NCORE=2, STEPS=4 | ~6.9 M | clean |
| NCORE=2, STEPS=5 | ~53 M | clean |
| NCORE=3, STEPS=3 | ~7.7 M | clean |

## Findings

`ASSUME_FIXES` (default 1) makes the confirmed SLICC gaps behave as their
natural fix so the search can go deeper. Set it to 0 to have the model abort at
the first reachable gap; fix-forward one at a time to walk F1–F8. Every finding
is a **confirmed reachable** counterexample (a concrete interleaving), each
documented inline at its fix site.

| # | kind | one-line |
|---|------|----------|
| F1 | panic | L2 `IM` has no `SPM_GETS_SILENT`/`SPMWB_REQ` transition (only `{IS,ISS}` stall them) — a store's memory fetch racing another core's SPM op to the line panics the L2 |
| F2 | panic | L1 `MX_L0`/`EX_L0` has no `WriteBack` transition — an L0 capacity eviction racing the `SPMCP_fetch` L0-recall panics the L1 |
| F3 | panic | L2 `SPM_WB` has no `L1_PUTX` transition — an owner writeback racing another core's `SPMWB_REQ` panics the L2 |
| F4 | panic | L2 `SPM_IS` has no `L1_PUTX` transition — a stale PUTX arriving during a silent memory fill panics the L2 |
| F5 | data-loss / panic | `SPMWB_REQ` runs `spc_clearSPMOwner` (wipes L2 sharer/owner) **without invalidating the L1 copies**; a concurrent sharer keeps a live copy → SWMR violation, and its later UPGRADE gets `DATA` in `SM` (no transition) → sequencer panic. Guarded by the software epoch contract, which the protocol does not enforce. |
| F6 | **deadlock** | The dying owner (`M_I`) serves `Fwd_GETS_Silent` from its writeback TBE and sends a plain `Unblock`, so L2 returns `MT_SPMS→MT` with a **dead owner**; the next request forwarded there hits `SINK_WB_ACK` and `Fwd_GETS`/`Fwd_GETX` panic while `Fwd_GETS_Silent` deadlocks. **Matches the documented PageRank "sequencer unanswered ~804K cycles" stampede.** |
| F7 | data-loss | `SPMCP_fetch` recall of an Exclusive source carrying an un-written-back dirty L0 store lands in `EE`-with-Dirty instead of `MM` (`EX_L0+L0_DataAck→EE`, L1cache.sm:1506, vs the normal `E_IL0→MM`); a later `Inv` (`EE+Inv→I` "don't send data") **silently drops the store** and L2 writes stale data to memory. No contract violation — a clean bug. |
| F8 | **deadlock** | Stale-sharer stampede: a core silently drops `SS→I` (stays a stale L2 sharer) then starts `SPMCP_fetch→IX_D`; another core's UPGRADE makes L2 `INV` the stale sharer and park in `SS_MB`, but `IX_D` **stalls Inv** (L1cache.sm:1367) while its own `GETS_SILENT` is queued behind that same block → circular wait. |
| F9 | panic (placement) | `SPMCP_install` whose target way aliases a coherent line in a **transient** state displaces it, hitting gem5's `allocateSPMSlot` `IsStableCoherent` assert. Reachable unless software guarantees install targets never alias a line with an outstanding coherence transaction (`INSTALL_STABLE_ONLY`). Found only by the unified coherence+placement model. |

### Suggested fixes (as modeled under `ASSUME_FIXES` / `INSTALL_STABLE_ONLY` / `MIN_FREE`)

- **F1–F4**: add the missing `stall_and_wait` / `WB_ACK` rows (park the SPM/PUTX
  event in the busy state), exactly as the neighboring transient states do.
- **F5**: `SPMWB_REQ` must either stall while coherent sharers/owner exist or
  invalidate them first, instead of silently clearing the directory metadata.
- **F6**: the `M_I` owner should hand its TBE data to L2 on `Fwd_GETS_Silent`
  (not a bare `Unblock`), and L2 `MT_SPMS + WB_Data → M` (owner has left), so no
  dead owner is left addressable.
- **F7**: `EX_L0` must resolve to `MM` when the recalled/absorbed data is dirty
  (mirroring `MX_L0` and the normal `E_IL0→MM` path), not unconditionally `EE`.
- **F8**: `IX_D` holds no coherent copy, so it can `InvAck` a stale-sharer
  invalidation immediately (stay `IX_D`) instead of stalling it.
- **F9 / no-free-way**: software must guarantee an install target way never
  aliases a coherence-transient line, and reserve ≥ `MIN_FREE` non-SPM ways so
  a displaced line always has a migration home (`MIN_FREE=2` gives headroom).

F6 and F8 are the two deadlocks; both are strong candidates for the open
`Fwd_GETS_Silent` stampede/deadlock tracked in the PageRank work.
