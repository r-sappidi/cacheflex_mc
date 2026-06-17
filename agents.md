# CacheFlex MC Agent Handoff

## Current Direction

Keep SPM work on `MESI_Three_Level_SPM` only. Do not reintroduce or depend on
`MESI_Two_Level_SPM`. Stock gem5 protocols/files may remain, but the custom
two-level SPM prototype has been removed.

## Ported Tooling And Benchmarks

- Added `spm_tools/spm_compiler.py`, adapted from `../cacheflex_micro` for this
  repo's ARM SPM encodings.
- Added `setup_spm_env.sh` to set `GEM5_SPM`, `GEM5_SE`, `SPM_COMPILER`,
  cross-compiler, and `m5op.o` paths.
- Added `benchmarks/spm_smoke/`:
  - `common_spm.hpp`
  - `spm_multicore_smoke.cpp`
  - `build.sh`
  - `run_mesi3_spm.sh`
  - `README.md`
- Built `gem5/util/m5/build/arm64/abi/arm64/m5op.o`.
- Built `benchmarks/spm_smoke/bin_gem5/spm_multicore_smoke`.

## Three-Level SPM Fixes Already Made

- Fixed ARM request flag truncation: SPM request flags live above 32 bits, so
  ARM memory instruction plumbing now uses `Request::FlagsType` instead of
  `unsigned`.
- Fixed O3 LSQ SPMCP metadata: for `SPMCP_64_IMM xDstSpm, [xSrc]`, the request
  address is the coherent source and the store data/register payload is the SPM
  destination.
- Removed all custom two-level SPM protocol/config/build files:
  - `gem5/src/mem/ruby/protocol/MESI_Two_Level_SPM*`
  - `gem5/src/mem/ruby/protocol/MESI_Two_Level_SPML1.sm`
  - `gem5/src/mem/ruby/protocol/MESI_Two_Level_SPML2.sm`
  - `gem5/configs/ruby/MESI_Two_Level_SPM.py`
  - `gem5/build_opts/ARM_MESI_Two_Level_SPM`
  - `gem5/build_opts/NULL_MESI_Two_Level_SPM`
- Made `MESI_Three_Level_SPM` self-contained by moving the needed SPM-aware
  `CoherenceRequestType`, `CoherenceResponseType`, `RequestMsg`, and
  `ResponseMsg` definitions into
  `gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-msg.sm`.
- Updated `gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM.slicc` so it no
  longer includes the deleted two-level SPM message file.
- Removed stale two-level SPM references from `SPM_MESI_THREE_LEVEL_DESIGN.md`.

## Verification Completed

- `python3 -m py_compile spm_tools/spm_compiler.py` passed.
- SPM compiler smoke produced expected `.inst` output for scalar SPM and SVE SPM
  forms.
- `bash benchmarks/spm_smoke/build.sh` builds the AArch64 smoke binary when
  `CROSS_CXX` points at the sibling `cacheflex_micro` toolchain.
- `find gem5 -maxdepth 4 -iname '*Two_Level*SPM*'` returned no files after
  cleanup.
- `rg 'MESI_Two_Level_SPM|Two_Level_SPML|RUBY_PROTOCOL_MESI_Two_Level_SPM' .`
  returned no references after cleanup.
- `scons build/ARM_MESI_Three_Level_SPM/gem5.opt -j4` completed successfully
  after the cleanup.

## GEMM Benchmark (added 2026-06-12)

`benchmarks/gemm/` is the current driver benchmark (SVE blocked GEMM). Two
variants built by `benchmarks/gemm/build.sh`, both run on the *same*
`ARM_MESI_Three_Level_SPM/gem5.opt` via `benchmarks/gemm/run_mesi3_baseline.sh`
(env `VARIANT=blocked|spm`):

- non-SPM = `gemm_blocked.cpp` -> `bin_gem5/gemm_blocked`
- SPM     = `gemm_spm.cpp` -> assembled, run through `spm_tools/spm_compiler.py`
  to encode the CacheFlex SPM pseudo-ops -> `bin_gem5/gemm_spm`

`gemm_spm.cpp` places the encoded SPM slot window at `kSpmWindowBase =
0x40000000` (1 GiB). Each thread stages the *same* `kc x nc` B panel into its
own private-L1 SPM and streams it with `spm.ld1w`. Because every thread stages
the same panel, multiple cores issue `SPMCP_fetch` on the *same* coherent source
lines concurrently (this is what surfaced the deadlock below).

Results live under `results/gemm/`. Old result dirs from 2026-06-12 predate the
deadlock work: `smoke_2c_m16k32n32` (non-SPM, completed); `spm_trace` (crashed,
OLD binary, `0x2480` address-truncation panic — that bug is fixed, see below);
`spm_smoke_*` SPM dirs (empty stats = the deadlock).

## Current Runtime Status (2026-06-13)

State of the SPM gemm path on `ARM_MESI_Three_Level_SPM`:

- **Single-core SPM gemm PASSES** end-to-end (`verify=pass`, clean exit).
  The SPM datapath (`SPMCP_fetch` -> X-slot install -> `spm.ld1w`) is correct.
  Repro (verified on the 2026-06-12 13:21 binary, before today's protocol edits;
  single-core does not exercise forwarding so it should be unaffected):
  ```bash
  G=gem5/build/ARM_MESI_Three_Level_SPM/gem5.opt
  S=gem5/configs/deprecated/example/se.py
  $G --outdir=/tmp/spm1 $S --ruby --num-cpus=1 --num-dirs=2 --num-l2caches=1 \
    --cpu-type=DerivO3CPU --sys-clock=1.5GHz --cpu-clock=1.5GHz --mem-size=2GB \
    --cacheline_size=64 --l0i_size=64kB --l0d_size=64kB --l0i_assoc=4 \
    --l0d_assoc=4 --l1d_size=512kB --l1d_assoc=8 --l2_size=4MB --l2_assoc=16 \
    -P "system.cpu[0].isa[0].sve_vl_se=4" \
    --cmd benchmarks/gemm/bin_gem5/gemm_spm \
    --options "--threads 1 --m 16 --k 64 --n 64 --kc 64 --nc 64 --warmup 0 \
               --repeat 1 --verify 16 --pin 1"
  ```
- **Multicore SPM gemm previously DEADLOCKED** (`Sequencer.cc:271 Possible
  Deadlock`) on concurrent same-line `SPMCP_fetch`. The fix is implemented (see
  next section) and **compiles cleanly (2026-06-13 15:48 binary)** but is
  **NOT YET RUNTIME-VALIDATED** — the 2-core run was not executed before the
  session ended. FIRST NEXT STEP: run the 2-core case and confirm it completes
  with `verify=pass`:
  ```bash
  # same as above but: --num-cpus=2, add -P system.cpu[1].isa[0].sve_vl_se=4,
  # and --threads 2 --m 16 --k 64 --n 64 --kc 64 --nc 64 --verify 16
  ```
- The earlier `0x2480` "Invalid transition" panic was a separate, already-fixed
  16-bit truncation of the SPM destination (`lsq.cc` reads the dst from register
  data; current binary installs slots at `0x40000000+` correctly). Not a concern.

## Concurrent SPM-Fetch Deadlock + Fix (2026-06-13, UNVALIDATED)

Root cause of the multicore deadlock: when two cores `SPMCP_fetch` the same line
and one owns it Modified, L2 routed the requester's `SPM_GETS_SILENT` through the
*normal* GETS path (`MT --SPM_GETS_SILENT--> MT_IIB`), which (a) waits for an
Unblock the silent requester never sends, and (b) would add the SPM requester as
a coherent sharer. Meanwhile the owner sits in `MX_A` (it sent `SPM_PUTM` for its
own fetch) and stalls the forwarded GETS waiting for its `Put_Ack`, while L2
(`MT_IIB`) stalls that `SPM_PUTM` waiting for the GETS to be answered -> 3-way
circular wait.

Key insight: the silent path was ALREADY correct for L2 states `NP/M/SS`
(`SPM_IS` state; `sds_/sdm_sendSPM...` deliver data with `AckCount:=0`, add no
sharer). Only the `MT` (owner-is-another-L1) case was broken. Fix makes `MT`
honor "silent" too.

Two distinct cases, two behaviors (this distinction matters — do not collapse
them): a **silent** forwarded GETS (peer `SPMCP_fetch`) must leave the owner's
coherence state UNCHANGED (snapshot only); a **normal** forwarded GETS (ordinary
coherent load) must DOWNGRADE the owner `M->S`. Going to `SX_A` is correct only
for the normal case; for the silent case the owner stays put, because the same
handler also serves plain `MM/EE` owners that have no outstanding PUT / `Put_Ack`
and would hang in `SX_A`.

Edits (all in `gem5/src/mem/ruby/protocol/`):

L2 (`MESI_Three_Level_SPM-L2cache.sm`):
- New state `MT_SPMS` ("MT, forwarded silent GETS, awaiting owner completion").
- `MT --SPM_GETS_SILENT--> MT_SPMS` (was `MT_IIB`).
- `sb_forwardSilentGetSToExclusive` now sends `CoherenceRequestType:GETS_SILENT`
  (was plain `GETS`) and carries `DstSPMAddr`.
- `MT_SPMS --Unblock--> MT`: owner signals completion; requester is NOT added as
  a sharer; line stays Modified in the owner.
- `MT_SPMS` added to the existing stall sets (L1-request / replacement / flush /
  MEM_Inv) so the owner's `SPM_PUTM` is held until the snapshot finishes (closes
  the race where the owner could vacate to `X` mid-forward).

L1 (`MESI_Three_Level_SPM-L1cache.sm`):
- New event `Fwd_GETS_Silent`; in_port maps `GETS_SILENT` to it (recalling L0
  first via `L0_Invalidate_Else` if the line is still resident there).
- New action `spm_sendSilentDataToRequestor` (DATA, `AckCount:=0`, carries
  `DstSPMAddr`) -> requester's `IX_D` sees `Data_all_Acks` -> `X`.
- `{MM, EE, MX_A, EX_A} --Fwd_GETS_Silent-->` (self-loop): send snapshot +
  `j_sendUnblock`, NO coherence-state change.
- `{SX_L0, MX_L0, EX_L0, IX_D, XWB} --Fwd_GETS_Silent-->` stall (z2); also added
  `Fwd_GETS_Silent` to the `{S_IL0,M_IL0,E_IL0,MM_IL0}` stall set.
- NEW (normal coherent GETS during an SPM fetch — a separate latent deadlock):
  `{MX_A, EX_A} --Fwd_GETS--> SX_A` doing `d_sendDataToRequestor; d2_sendDataToL2`
  (mirrors `MM/EE --Fwd_GETS--> SS`). `MX_A/EX_A` were split out of the old
  `{...} {Fwd_GETS,Fwd_GETX,Inv}` stall block; they still stall `Fwd_GETX/Inv`.

Known still-open / not done:
- `Fwd_GETX`/`Inv` arriving at `MX_A/EX_A` (a coherent *store* by another core
  racing an SPM fetch) still STALL -> potential latent deadlock, same shape as
  the GETS case. Not yet handled; needs the owner to relinquish fully while still
  completing its own SPM install. Revisit if a workload hits it.
- >2-core gemm runtime validation, and SPM-vs-non-SPM stat collection, remain TODO.

## 2-core Validation + Stall/Wakeup Fix (2026-06-14, VALIDATED)

The 15:48 (2026-06-13) binary still DEADLOCKED at 2 cores (`Sequencer.cc:271`,
CPU0 read of paddr `0xc81c0` stuck). Protocol trace (`results/gemm/spm_2c_trace/
ptrace.txt`, traced from tick 133M) showed the silent-forward fix itself works
(line `0xc8180` completed on both cores), but exposed a stall/wakeup ordering bug:

- When a peer's `Fwd_GETS_Silent` arrives while the owner is still recalling L0
  for its OWN fetch (state `MX_L0`), it is stalled by `z2_stallAndWaitL2Queue`.
- The owner's `MX_L0 --L0_DataAck--> MX_A` transition did NOT wake the stalled
  forward queue, so once in `MX_A` the silent forward was never replayed. L2 sat
  in `MT_SPMS` waiting for the owner's Unblock, the owner's own `SPM_PUTM` was
  stalled by `MT_SPMS`, and the peer's `IX_D` never got data -> 3-way deadlock.

Fix (L1cache.sm): added `kd_wakeUpDependents;` to the `*X_L0 -> *X_A`
recall-completion transitions (`SX_L0->SX_A`, `EX_L0->EX_A`, both `MX_L0->MX_A`).
Harmless no-op when nothing is stalled. Rebuilt 2026-06-14 16:25.

Result: 2-core `gemm_spm` (`--threads 2 --m 16 --k 64 --n 64 --kc 64 --nc 64
--verify 16`) now PASSES: `verify=pass checksum=0.341476023`, clean exit at tick
157878900, full stats in `results/gemm/spm_2c_validate2/`.

## Useful Commands

Build gem5:

```bash
cd gem5
scons build/ARM_MESI_Three_Level_SPM/gem5.opt -j4
```

Build smoke benchmark:

```bash
CROSS_CXX=/home/rsappidi/research/cacheflex_micro/tools/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++ \
  bash benchmarks/spm_smoke/build.sh
```

Run small smoke:

```bash
bash benchmarks/spm_smoke/run_mesi3_spm.sh 2 1 2 128
```

## Environment Notes

- Some local commands intermittently fail under the sandbox wrapper with:
  `bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted`.
- In prior runs, rerunning the same local command with escalation worked.
- The sibling repo `../cacheflex_micro` is useful for toolchain binaries and
  historical compiler tooling, but do not port its single-core gem5/two-level
  SPM protocol path back into this repo.
