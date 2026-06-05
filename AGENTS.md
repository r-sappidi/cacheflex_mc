# Repository Guidelines

## Project Structure & Module Organization

This repository wraps a gem5 checkout plus CacheFlex SPM protocol notes. The root contains `SPM_GEM5_GUIDE.md`, `SPM_RUBY_IMPLEMENTATION_PLAN.md`, and the coherency state tables: `spm_coherency_CC.csv` and `spm_coherency_dir.csv`. gem5 source lives under `gem5/src`, with Ruby protocols in `gem5/src/mem/ruby/protocol`. Configuration scripts are in `gem5/configs`, build options in `gem5/build_opts`, and tests in `gem5/tests`. Generated build output belongs under `gem5/build` and should not be edited by hand.

## Build, Test, and Development Commands

Run commands from `gem5/` unless noted.

- `scons build/ARM/gem5.opt -j$(nproc)`: build an optimized ARM gem5 binary.
- `scons build/ALL/gem5.opt -j$(nproc)`: build with broad ISA/protocol coverage when testing protocol changes.
- `scons defconfig build/ARM build_opts/ARM`: refresh Kconfig settings from a build option file.
- `scons defconfig build/ARM_MESI_Two_Level_SPM build_opts/ARM_MESI_Two_Level_SPM`: configure the two-level CacheFlex SPM protocol fork.
- `scons defconfig build/ARM_MESI_Three_Level_SPM build_opts/ARM_MESI_Three_Level_SPM`: configure the three-level CacheFlex SPM protocol fork.
- `scons build/ARM_MESI_Three_Level_SPM/gem5.opt -j$(nproc)`: build the three-level CacheFlex SPM target.
- `./build/ARM/gem5.opt configs/example/se.py --help`: sanity-check the built binary and config wiring.
- `python3 tests/main.py run gem5/quick`: run gem5 quick tests, if the local test dependencies are installed.

## Coding Style & Naming Conventions

Follow gem5’s existing style in nearby files. C++ and SLICC use 4-space indentation, descriptive enum/action names, and gem5 naming patterns such as `CamelCase` types, `lowerCamelCase` methods, and protocol events like `SPMWB_Ack`. Keep SLICC actions small and name transitions after protocol-visible behavior. CacheFlex SPM protocol work currently lives in explicit forks: `MESI_Two_Level_SPM.slicc`, `MESI_Two_Level_SPML1.sm`, `MESI_Two_Level_SPM-msg.sm`, and the three-level fork `MESI_Three_Level_SPM.slicc`, `MESI_Three_Level_SPM-L0cache.sm`, `MESI_Three_Level_SPM-L1cache.sm`, `MESI_Three_Level_SPM-L2cache.sm`, `MESI_Three_Level_SPM-dir.sm`, and `MESI_Three_Level_SPM-msg.sm`. Do not silently modify stock `MESI_Two_Level.slicc` or stock three-level protocol files unless the user asks to merge a fork.

## Testing Guidelines

For Ruby protocol edits, test both compilation and a small simulation. At minimum, rebuild the affected target and run a simple SE workload using the intended Ruby protocol. Add or update tests under `gem5/tests` when behavior is reusable. Use focused protocol traces or debug flags when validating state-machine behavior; keep generated logs out of commits.

For CacheFlex SPM checkpoint work, scale validation to the scope of the change:

- Static/readback checks are acceptable for small local SLICC action or transition edits.
- Run `scons defconfig build/ARM_MESI_Two_Level_SPM build_opts/ARM_MESI_Two_Level_SPM` when config or protocol-selection files change.
- Batch full `scons build/ARM_MESI_Two_Level_SPM/gem5.opt` validation for `C13` or after several SLICC checkpoints, unless a change touches shared message/type definitions or likely breaks SLICC generation.
- A successful build only proves SLICC/generated C++ integration; it does not prove base L1 behavior or SPM protocol correctness.
- Run a simple non-SPM Ruby SE workload to check that base L1/L2 behavior still works.
- Run SPM-specific simulations for the three-level fork after porting workload/runtime support from `../cacheflex_micro`; the protocol-side path for `GETS_SILENT`, `PUTS`, `PUTM`, `PUTE`, `PUT_ACK`, `SPMWB_REQ`, and `SPMWB_ACK` is now implemented in `MESI_Three_Level_SPM`.

## Commit & Pull Request Guidelines

The root history currently only has an initial commit, so use concise imperative commit subjects, for example `Add SPM Ruby request types`. Keep each commit focused on one behavior or refactor. Pull requests should include a short summary, affected files or protocol states, commands run, and any known limitations. Link issues or notes when changing behavior described by `SPM_GEM5_GUIDE.md` or the CSV state tables.

## Agent-Specific Instructions

Before changing coherency behavior, read both CSV state tables and `SPM_GEM5_GUIDE.md`. The current modeling decision is that CacheFlex SPM data is implemented in the core's private cache/L1 protocol, not as repurposed shared L2 ways. Treat the CSV `CC` table as L1/private-cache behavior and the CSV `Dir` table as directory-controller behavior. Document which controller owns each state transition.

Before changing the three-level implementation, also read `SPM_MESI_THREE_LEVEL_DESIGN.md`. In gem5 three-level naming, `L0Cache` is core-facing private I/D, `L1Cache` is the private home for physical SPM ways and owns `X`, `L2Cache` is the on-chip coherence home for source-line Dir-table behavior, and `Directory` is memory-side. The three-level SPM fork is intended to keep SPM data in the private `L1Cache`, not in shared L2 ways.

`../cacheflex_micro` contains another gem5 checkout with SPM workload/runtime support and benchmarks. When adding executable SPM validation here, inspect that tree first for ISA hooks, syscall/runtime glue, benchmark sources, build scripts, and run scripts that can be ported into this repository. Treat it as a workload/reference source, not as permission to overwrite this repo's protocol model.

## CacheFlex SPM Protocol Notes

Treat the CSV `Dir` table as the directory-controller behavior, not the L1/private-cache SPM controller. Earlier notes about repurposed shared L2 ways are stale for this implementation. For this repo, `SPMCP_install` claims a private-cache/L1 slot as SPM state `X`; any later implementation of destination-slot allocation must stay local to the private cache and must not be modeled as a coherence eviction.

Keep `SPMCP_install` distinct from `SPMCP_fetch`. `SPMCP_fetch` copies a source line into the SPM slot and removes the source line from coherence. If the source is in `S`, `M`, or `E`, follow the CC table by sending `PutS`, `PutM + data`, or `PutE` and waiting for `Put-Ack` before installing `X`. If the source is absent, issue `GetS_silent`; the returned data is routed to SPM and the requester is not added as a sharer.

SPM operations use two address spaces. The source address is a normal coherent address. The destination is an SPM slot encoded as `(way_id << 16) | (set_index << 6) | byte_offset`, as described in `SPM_GEM5_GUIDE.md`. Ruby requests/TBEs for SPM copy need to preserve both the coherent source address and destination SPM set/way.

`MESI_Two_Level_SPML1.sm` has been rewritten as a CacheFlex L1 state-machine specification based on the CC table. It includes SPM states/events for `SPMCP_fetch`, `SPMCP_install`, `SPMLD`, `SPMST`, `SPMWB_read`, `SPMWB_store`, and `SPM_release`. Ruby request types and dual-address metadata have partial plumbing in `Request`, `RubyRequest`, `RubySlicc_Exports.sm`, and `Sequencer.cc`, but the L1 state machine still needs the remaining checkpoints below before it should be treated as complete. `MESI_Two_Level_SPM-msg.sm` now declares `GETS_SILENT`, `PUTS`, `PUTM`, `PUTE`, `PUT_ACK`, `SPMWB_REQ`, and `SPMWB_ACK`; keep enum spelling consistent.

`MESI_Three_Level_SPM` is the active three-level fork. It includes build/config wiring in `gem5/build_opts/ARM_MESI_Three_Level_SPM`, `gem5/configs/ruby/MESI_Three_Level_SPM.py`, and `gem5/src/mem/ruby/protocol/Kconfig`. `MESI_Three_Level_SPM-msg.sm` adds L0↔L1 SPM request classes and preserves dual-address metadata (`SrcAddr`, `DstSPMAddr`, `SPMSet`, `SPMWay`, `Len`). `MESI_Three_Level_SPM.slicc` includes `MESI_Two_Level_SPM-msg.sm` for shared Ruby request/response enums, so keep enum spelling consistent across both forks.

Three-level SPM implementation status as of May 29, 2026:

- `L0Cache` forwards `SPMCP_fetch`, `SPMCP_install`, `SPMLD`, `SPMST`, `SPMWB_read`, `SPMWB_store`, and `SPM_release` to private `L1Cache`, and completes sequencer callbacks on `SPM_DATA`/`SPM_ACK`.
- `L1Cache` owns SPM state `X`, claims destination slots with `CacheMemory::allocateSPMSlot`, returns zero for non-`X` SPM reads, updates `X` data on `SPMST`, releases `X` on `SPM_release`, and protects `X` from normal replacement/forwarded coherence.
- Three-level `SPMCP_fetch` keeps coherent source addresses separate from encoded SPM destination addresses. Absent sources use `GETS_SILENT` and install from TBE data. Resident `SS`/`EE`/`MM` sources issue `PUTS`/`PUTE`/`PUTM`; resident `S`/`E`/`M` sources first recall/invalidate L0, then issue the corresponding `PUT*`. On `PUT_ACK`, L1 installs the destination `X` entry and deallocates the coherent source.
- `SPMWB_store` from `X` now sends `SPMWB_REQ` toward the L2/directory home and completes only after `SPMWB_ACK`.
- `L2Cache` implements the Dir-table home behavior for `GETS_SILENT`, `PUTS`, `PUTM`, `PUTE`, `PUT_ACK`, `SPMWB_REQ`, and `SPMWB_ACK`, including silent data return without adding the requester as a sharer.
- `Directory` implements memory-side handling for silent fetch, source-release writeback/acks, and SPM writeback-to-memory acks.

Validated commands for the three-level fork:

- `scons build/ARM_MESI_Three_Level_SPM/gem5.opt -j$(nproc)`
- `./build/ARM_MESI_Three_Level_SPM/gem5.opt configs/deprecated/example/se.py --ruby --cpu-type=ArmTimingSimpleCPU --cmd tests/test-progs/hello/bin/arm/linux/hello`

The non-SPM Ruby SE smoke test prints `Hello world!` and the simulated process exits with code 13. SPM-specific workload validation is still pending workload/runtime porting from `../cacheflex_micro`.

## Scope: Correctness Testbed, Not a Perf Model

This Ruby SPM implementation targets **protocol correctness first**. It is not intended to match the geometry assumptions in `SPM_GEM5_GUIDE.md` (192KB SPM, 1024 L2 sets) or to deliver performance numbers yet. The three-level fork is now the place to validate the intended L0/L1/L2 split, but do not raise concerns about L1 capacity, address-decoding mismatches, or per-set non-SPM-way availability as blockers for correctness validation. `SPM_RUBY_IMPLEMENTATION_PLAN.md` §5 (L2-way modeling) is superseded by the L1-private decision recorded here.

## Protocol Model vs. Implementation Plumbing

The CC table models the SPM transition as a single line at this L1 going `I/S/E/M → (transient) → X`, where `X` is the SPM state. The state `X` is **specific to the L1 controller's SLICC state machine** — it is declared per-controller in `state_declaration(State, ...)`, has its own protocol-defined transitions for SPM events, and projects to `AccessPermission:Read_Write` at the `AbstractCacheEntry` level (indistinguishable from `M` via `m_Permission` alone). The base class has no notion of "`X`"; shared infrastructure such as `CacheMemory` must use the `m_isSpm` bit (see below) to distinguish SPM entries from coherent ones.

`X`-specific behavior must be preserved against the CC table. Concretely:

- `X, SPMLD`/`SPMWB_read` → return SPM data, stay in `X`.
- `X, SPMST` → update SPM data in place, stay in `X`.
- `X, SPMWB_store` → emit `SPMWB_REQ`, transition to `XWB`.
- `XWB, SPMWB_Ack` → return to `X`.
- `X, SPM_release` → drop SPM data, deallocate, transition to `I`.
- `X, Fwd_GETS/Fwd_GETX/Fwd_GET_INSTR/Inv` → stall (the SPM line is outside the coherence domain).

Outside-`X` rows (`{NP, I, S, E, M}`) for SPM events deliver the CC table's `Return zero` / `Ignore` semantics through dedicated SLICC transitions; do not collapse these into the `X` transitions.

Empty CC-table cells (`X, SPMCP_fetch`, `X, SPMCP_install`) are intentionally unhandled — SLICC raises an unexpected-event error if they ever fire, which matches the table's "shouldn't happen" intent. Software should `SPM_release` before re-installing a slot.

The implementation cannot literally retag the source entry as `X` because the SPM ISA addresses the resulting SPM line via a separate encoded VA (`(way<<16) | (set<<6) | offset`), and the cache is tag-indexed. Instead, `SPMCP_fetch` produces **two physical entries** during its lifecycle: the source entry (at the coherent source VA) is fully deallocated by `ff_deallocateL1CacheBlock` at install time, and a new entry is created at the encoded destination VA in state `X` with `setScratchpad(true)`. The CC-table-level abstraction "source line transitions to `X`" corresponds physically to "old tag goes, new tag at the encoded SPM VA carries the same data in state `X`." Outcome is the same; only the bookkeeping differs because of the encoded-VA addressing scheme.

After a successful fetch, the source coherent VA has no entry at this L1 (lookup misses, defaults to `I`), and the directory has cleared this L1 from sharers/owner via `PutS`/`PutM`/`PutE` + `Put_Ack`. Both sides see the source as coherence `I`. There is no SPM-flavored state at the source VA.

## Source Data Buffering During SPMCP_fetch

`SPMCP_fetch` dispatches at the source coherent VA and waits for coherence acks/data before installing into the SPM slot at the destination encoded VA. The data path during the wait depends on the source's stable state:

- Source `I`/`NP` (path `IX_D`): `GETS_SILENT` is sent; the returned data is **buffered in the TBE** (`tbe.DataBlk`) by `spm_bufferResponseData` and then installed via `spm_installFromTBE` at response-arrival time. The source was never resident locally.
- Source `S`/`E`/`M` (paths `SX_A`/`EX_A`/`MX_A`): the source entry stays resident in its transient state through the ack wait. The TBE is **not** used as a data buffer in these paths; `spm_installFromCache` reads `cache_entry.DataBlk` directly from the still-resident source entry at `Put_Ack` arrival, then `ff_deallocateL1CacheBlock` evicts it.

The destination slot is not consulted at fetch dispatch — only at install time. Fetch and migration are independent: the destination slot remains a normal coherent slot through the entire fetch wait and could even be picked as a normal-replacement victim by an unrelated miss during that window. Migration runs synchronously inside `CacheMemory::allocateSPMSlot` at install. Consequence: a failure-to-migrate (no eligible non-SPM way) panics at install time, after source-side coherence work has already been done; this is a deliberate simplification for the testbed.

## `m_isSpm` on AbstractCacheEntry

The SPM marker bit lives on `AbstractCacheEntry` (`m_isSpm` field + `setScratchpad(bool)` / `isScratchpad() const` methods). This is the **single source of truth** for "is this slot SPM?" — the previous SLICC-side `isSPM` field on the L1 `Entry` struct was removed to avoid dual-state. The bit was lifted to the base class because `CacheMemory` only holds `AbstractCacheEntry*` pointers and cannot downcast to a protocol-specific `Entry` without coupling shared infrastructure to one protocol. `CacheMemory::allocateSPMSlot` sets the bit; `CacheMemory::migrateOrClearSPMSlot` reads it to identify SPM ways and to assert against accidentally relocating an SPM occupant. SLICC actions write through `cache_entry.setScratchpad(true/false)` (same dispatch idiom as `cache_entry.changePermission(...)`).

## Rumur/Murphi Verification Notes

Rumur verification artifacts live in `verification/rumur`. Run `./run_cacheflex_spm.sh` from that directory. The current setup has two models:

- `cacheflex_spm_tables_complete.m`: generated by `gen_table_model.py` from `spm_coherency_CC.csv` and `spm_coherency_dir.csv`; it emits one rule for each non-empty CSV cell and deliberately emits no rule for blank cells.
- `cacheflex_spm_2core.m`: a hand-written 2-core, one-line table-level safety model for L1 + directory SPM behavior.

The current 2-core Rumur model checks protocol-level safety around SPM `X`: an SPM line is not a directory sharer or owner, directory bookkeeping remains consistent, and two cores cannot both be stable `E/M` owners. It does **not** yet verify lazy destination-slot migration at the physical set/way level. In particular, it does not model a destination SPM slot that already contains a coherent occupant and then prove that `SPMCP_install` relocates that occupant to another non-SPM way without emitting coherence messages or changing directory ownership/sharer state.

Next Rumur step: extend the hand-written 2-core model with a tiny set-associative L1, e.g. one set with 2-3 ways per core, per-way `{valid, addr_id, cc_state, is_spm}` metadata, and directory metadata per coherent address. Model `SPMCP_install(dst_way)` explicitly as lazy migration: if the destination way contains a coherent non-SPM line, move that exact line/state/data identity to a free non-SPM way in the same set, assert directory state is unchanged, assert no Put/Inv/WB-style coherence side effect is emitted, panic/fail if no non-SPM way exists, and reject installing over an existing SPM `X` slot.

## SPML1 Checkpoint Status

- `C7`: Complete request completion semantics. Implement explicit zero-data read callbacks for `SPMLD`/`SPMWB_read` outside `X`, complete ignored stores/writebacks/releases through the sequencer, and audit every SPM transition that pops the mandatory queue for a matching callback. Completed in `MESI_Two_Level_SPML1.sm`; full build validated under `C13`.
- `C8`: Fix SPM address ownership. Keep coherent source addresses separate from encoded SPM slot addresses. `SPMCP_fetch` is keyed by the coherent source while installing into `DstSPMAddr`; `SPMLD`, `SPMST`, `SPMWB_read`, and `SPM_release` operate on the SPM slot; `SPMWB_store` must send the coherent writeback address, not accidentally the SPM slot key. Completed in `MESI_Two_Level_SPML1.sm`; directory-side ack routing remains part of the later directory checkpoint.
- `C9`: Implement destination slot claiming. Replace normal cache allocation for `SPMCP_install` with private-cache SPM slot selection using decoded `SPMSet`/`SPMWay`, mark the resulting entry as stable `X`, and keep it outside the coherence domain. Completed via the `spm_claimSlot` action plus `CacheMemory::allocateSPMSlot` (asserts decoded set matches `addressToCacheSet`, asserts way is within associativity, panics if the slot holds a different coherent line). Out-of-coherence treatment relies on the disjoint SPM VA convention, state `X` having no `Load`/`Store`/`Ifetch` transitions, and C11 replacement/probe guards.
- `C10`: Implement local coherent-occupant migration for `SPMCP_install` over `S`, `E`, or `M`. Move the existing coherent line to an available non-SPM way in the same private-cache set without modeling a coherence eviction; fail clearly if no legal non-SPM way exists. Completed via `CacheMemory::migrateOrClearSPMSlot`, invoked from `allocateSPMSlot` before the slot is claimed. The migration is pure physical relocation — the occupant's coherence state and data block travel with the entry; no coherence message is emitted. The SPM marker bit was lifted from the SLICC `Entry` struct down into `AbstractCacheEntry` (`m_isSpm` + `setScratchpad`/`isScratchpad`) so `CacheMemory` can identify SPM ways without downcasting to a protocol-specific Entry. Panics if no eligible non-SPM way is available or if the target slot already holds an SPM line (re-install without `SPM_release`).
- `C11`: Protect SPM entries from normal replacement. Completed via `CacheMemory` scratchpad guards for `cacheAvail`, `allocate`, `deallocate`, and `cacheProbe`, plus explicit `X` handling for `L1_Replacement` and `PF_L1_Replacement`.
- `C12`: Harden transient and race behavior. Implemented SPM transient stalls, invalidation handling for source-removal transients, and owner-sourced data routing for `IX_D`; further race validation should continue with focused simulations after directory support lands.
- `C13`: Make the L1 fork SLICC/build clean. Completed on May 26, 2026: fixed the SPM ARM build option to use `BUILD_ISA=y` and `USE_ARM_ISA=y`, ran `scons defconfig build/ARM_MESI_Two_Level_SPM build_opts/ARM_MESI_Two_Level_SPM`, rebuilt `build/ARM_MESI_Two_Level_SPM/gem5.opt`, and ran a non-SPM Ruby SE `hello` smoke test. The host gem5 run completed and printed `Hello world!`; the simulated process reported exit code 13.
- `C14`: Add focused validation hooks. Add useful `DPRINTF` traces and assertions for SPM metadata validity, `X`/`isSPM` consistency, stale metadata after release/migration, and invalid destination set/way values.

The next two-level L1 checkpoint is `C14`: add focused validation hooks.

For the three-level fork, the next practical checkpoint is executable SPM validation: inspect `../cacheflex_micro`, identify the minimum ISA/runtime/benchmark pieces needed to issue `SPMCP_fetch`, `SPMCP_install`, `SPMLD`, `SPMST`, `SPMWB_read`, `SPMWB_store`, and `SPM_release`, then port the smallest workload path into this repo. Do not reintroduce an L2-way SPM model unless the user explicitly changes the modeling decision.
