# CacheFlex SPM on gem5 MESI Three Level: Full Implementation Design

## Purpose

This document designs a performance-oriented CacheFlex SPM implementation on top of gem5's `MESI_Three_Level` Ruby protocol. The naming mismatch is important: `SPM_GEM5_GUIDE.md` describes SPM as repurposed L2 ways in the architectural guide, but gem5's three-level Ruby protocol names the core-facing private caches `L0`, the private last-level cache `L1`, and the shared/memory-side level `L2`. Therefore, the guide's SPM-backed L2 ways map to gem5 `L1Cache` ways in this design.

The protocol stack is:

- `L0Cache`: core-facing private instruction/data caches.
- `L1Cache`: private last-level cache for the core cluster; this owns physical SPM ways.
- `L2Cache`: on-chip coherence home / shared backing level in the stock protocol.
- `Directory`: memory-side controller.

The current repository's `MESI_Two_Level_SPM` prototype is aligned with this physical decision because it keeps SPM in the private cache controller. The three-level design should preserve that core idea while adapting it to the L0/L1 split and the stock three-level coherence path.

## Source of Truth

The protocol behavior is defined by:

- `spm_coherency_CC.csv`: private-cache/core-visible SPM behavior.
- `spm_coherency_dir.csv`: coherence-home behavior for removing coherent source ownership and handling silent fetch/writeback requests.
- `SPM_GEM5_GUIDE.md`: ISA, SPM address layout, benchmark usage, and the required 1024-set, 8-way, 64 B guide mapping.
- Existing SPM plumbing in `RubyRequest`, `RubySlicc_Exports.sm`, `RubySlicc_Types.sm`, `Sequencer.cc`, `AbstractCacheEntry`, and `CacheMemory`.

`SPM_RUBY_IMPLEMENTATION_PLAN.md` and the existing `MESI_Two_Level_SPM*` files are implementation references, not final three-level protocol files.

## Resolved Design Decisions

These clarifications are now baked into the design:

- Guide "L2 ways" mean gem5 `MESI_Three_Level` private `L1Cache` ways.
- SPM storage is physically private in gem5 `L1Cache`, the last private cache before the shared/home level.
- `SPMWB_store` is an SPM-to-memory writeback operation and should not interact with active coherent lines. If the target is still a coherent memory address with active coherence state, the implementation should return zeroes/complete benignly instead of performing a coherent write transaction.
- `SPMCP_install` migration failure does not need detailed performance modeling. Software guarantees at least two non-SPM ways are available for migration.
- The SPM set/way decode follows the guide's fixed mapping: 1024 sets, 8 ways, 64 B lines.

## Remaining Clarification

One architectural choice still needs a performance-model decision:

- `spm_coherency_dir.csv` should be implemented at the controller that represents the real coherence home. In stock `MESI_Three_Level`, `L2Cache` is the practical on-chip sharer/owner tracker while `Directory` is memory-side. For a realistic performance model, the CSV Dir behavior should usually live in `L2Cache`, with `Directory` only used when an operation genuinely reaches memory. Implementing it literally in `Directory` would add memory-side traffic to ownership transitions that a real on-chip coherence home would normally handle locally.

## Protocol Fork

Create a dedicated protocol fork instead of modifying stock `MESI_Three_Level` in place:

- `MESI_Three_Level_SPM.slicc`
- `MESI_Three_Level_SPM-msg.sm`
- `MESI_Three_Level_SPM-L0cache.sm`
- `MESI_Three_Level_SPM-L1cache.sm`
- `MESI_Three_Level_SPM-L2cache.sm`
- `MESI_Three_Level_SPM-dir.sm`
- `MESI_Three_Level_SPM-dma.sm` if DMA behavior diverges from stock

Target include graph:

```slicc
protocol "MESI_Three_Level_SPM" use_secondary_load_linked,
    use_secondary_store_conditional, supports_flushes;
include "MESI_Three_Level_SPM-msg.sm";
include "MESI_Three_Level_SPM-L0cache.sm";
include "MESI_Three_Level_SPM-L1cache.sm";
include "MESI_Three_Level_SPM-L2cache.sm";
include "MESI_Three_Level_SPM-dir.sm";
include "MESI_Two_Level-dma.sm";
```

## Address Model

SPM operations use two address spaces:

- **Coherent source address:** normal physical line address in the MESI coherence domain.
- **Encoded SPM slot address:** `(way_id << 16) | (set_index << 6) | byte_offset`.

The implementation must preserve both addresses in `RubyRequest`, L0-to-L1 messages, L1-to-home messages, responses, and TBEs:

- `SrcAddr`: coherent source for `SPMCP_fetch`; for `SPMWB_store`, this field is diagnostic only unless a memory-side writeback path is explicitly selected.
- `DstSPMAddr`: encoded SPM slot address.
- `SPMSet`: decoded set index, guide bits `[15:6]`, range `0..1023`.
- `SPMWay`: decoded way id, guide bit field beginning at bit `16`, range constrained by configured SPM ways and 8-way guide geometry.
- `Len`: operation length.
- `DataBlk`: line payload when buffered.

`DstSPMAddr` is a private-cache placement selector. It must not be treated as a normal coherent address by L2 or Directory.

## Message Types

Reuse the existing SPM request spelling:

Requests:

- `GETS_SILENT`: fetch data for SPM without adding the requester as a sharer.
- `PUTS`: remove a shared source copy before SPM install.
- `PUTM`: remove a modified source copy and write data back before SPM install.
- `PUTE`: remove an exclusive source copy before SPM install.
- `SPMWB_REQ`: memory-side SPM writeback request, only for the non-coherent SPM writeback path.
- `SPM_INSTALL`: L0/L1 request to claim a private `L1Cache` SPM slot.
- `SPM_LD`: private SPM slot read.
- `SPM_ST`: private SPM slot write.
- `SPM_RELEASE_REQ`: release a private SPM slot.

Responses:

- `PUT_ACK`: source removal is visible at the coherence home.
- `SPMWB_ACK`: SPM writeback reached memory/home, if that path is used.
- `SPM_DATA`: data returned from a private SPM slot.
- `SPM_ACK`: non-data SPM operation completed.

The new local SPM messages are between `L0Cache` and `L1Cache`. `GETS_SILENT`, `PUTS`, `PUTM`, `PUTE`, and `PUT_ACK` travel between `L1Cache` and the coherence home (`L2Cache` in the recommended performance model).

## Processor Request Mapping

Reuse existing processor-originated request types:

- `SPMCP_fetch`
- `SPMCP_install`
- `SPMLD`
- `SPMST`
- `SPMWB_read`
- `SPMWB_store`
- `SPM_release`

`Sequencer.cc` already has partial recognition and metadata propagation for these types. In three-level Ruby, the mandatory queue is consumed by `L0Cache`, so `L0Cache` must forward SPM metadata to `L1Cache` instead of trying to allocate SPM state locally.

## Controller Responsibilities

### L0Cache

L0 is the CPU-facing completion point. It does not persistently own SPM data.

Responsibilities:

- Recognize SPM request types from the mandatory queue.
- Forward SPM requests to private L1 with `SrcAddr`, `DstSPMAddr`, `SPMSet`, `SPMWay`, length, write mask, and data metadata intact.
- Complete sequencer callbacks:
  - `SPMLD` and `SPMWB_read`: return private SPM data, or zero when the slot is absent.
  - `SPMST`, `SPMCP_install`, `SPMCP_fetch`, `SPMWB_store`, `SPM_release`: complete as write-like operations once an ack arrives.
- Maintain per-slot ordering for SPM requests from the same core.
- Continue to rely on ISA `dsb sy` and the O3 shadow table for cross-address-space SPMCP-to-SPMLD ordering.

L0 should not allocate `X` as a normal cache line. A future optional non-coherent SPM micro-cache can be added later, but it must be explicitly invalidated on `SPM_release` and overwritten `SPMCP_install`.

### L1Cache

L1 is the private physical SPM owner and the controller that owns state `X` from the CC table.

Stable states add:

- `X`: private SPM slot; outside the coherence domain.

Transient SPM states mirror `spm_coherency_CC.csv`:

- `IX_D`: source absent locally; issued `GETS_SILENT`; waiting for data.
- `SX_A`: source was shared; sent `PUTS`; waiting for `PUT_ACK`.
- `MX_A`: source was modified; sent `PUTM + data`; waiting for `PUT_ACK`.
- `EX_A`: source was exclusive; sent `PUTE`; waiting for `PUT_ACK`.
- `XWB`: SPM writeback request outstanding, if the memory writeback path is used.

Responsibilities:

- Preserve stock three-level MESI behavior for normal `Load`, `Store`, `Ifetch`, replacement, flush, and LL/SC paths.
- Implement all `X` rows from `spm_coherency_CC.csv`.
- Implement outside-`X` SPM rows from the CC table: zero-return reads and ignored stores/releases complete through the sequencer.
- Claim physical SPM ways using guide set/way decode.
- Migrate a coherent occupant of the destination way to a free non-SPM way in the same set without coherence messages.
- Exclude SPM entries from normal replacement, deallocation, and probe candidates.
- Stall forwarded coherent requests for `X` lines and for source-removal transients according to the CC table.

For `S/E/M` source states where the freshest copy may live in L0, L1 must first recall/invalidate L0 using the existing L0 invalidation path, then send `PUTS`, `PUTE`, or `PUTM` to the home. SPM install happens only after `PUT_ACK`.

For `NP/I` source state, L1 sends `GETS_SILENT` to the home. Returned data is installed into the destination SPM slot and is not allocated as a normal coherent L1 line.

### L2Cache

L2 is the recommended home for `spm_coherency_dir.csv` behavior in a realistic gem5 performance model because it already tracks on-chip sharers and exclusive owners for `MESI_Three_Level`.

Responsibilities:

- Preserve normal stock L2 behavior for non-SPM traffic.
- Accept `GETS_SILENT`, `PUTS`, `PUTM`, `PUTE`, and `SPMWB_REQ` from private L1s.
- Implement the Dir table's `I/S/E/M/SD` behavior against L2's sharer and exclusive-owner metadata.
- Forward to another private L1 when owner-sourced data is required.
- Route silent data back to the SPM requester without adding that requester as a sharer.
- Send `PUT_ACK` after source removal is complete.
- Use Directory/memory only for true memory fetches and writebacks.

Recommended L2 Dir-table mapping:

- `GETS_SILENT` in `I`: fetch/send data and do not set owner or sharer.
- `GETS_SILENT` in `S`: send clean data and do not modify the sharer list.
- `GETS_SILENT` in `E/M`: forward to owner, make the old owner a sharer, do not add the SPM requester as sharer, clear owner, record `TBE.silent = true`, and enter `SD`.
- `PUTS`: remove requester from sharers; send `PUT_ACK`; transition to `I` when last sharer leaves.
- `PUTM`: if from owner, copy data to L2/memory as needed, clear owner, send `PUT_ACK`; if from non-owner, ack without changing owner.
- `PUTE`: if from owner, clear owner and send `PUT_ACK`; if from non-owner, ack without changing owner.
- `SPMWB_REQ`: should normally be unused for coherent destinations. If received for an address that is still coherent/owned/shared, return zero/benign completion rather than performing a coherence write.

### Directory

Directory remains memory-side. It should not own encoded SPM slot state.

Responsibilities:

- Serve memory reads for `GETS_SILENT` misses that L2 cannot satisfy on chip.
- Accept normal writeback traffic from L2.
- Optionally accept `SPMWB_REQ` only for a non-coherent memory-side writeback path.
- Never add an SPM requester as a coherent sharer.

If the implementation team chooses literal `Directory` ownership for the Dir CSV table instead of L2, document the added traffic and latency explicitly because that would be less representative of an on-chip coherence-home implementation.

## L1 SPM Storage

Add or reuse per-entry metadata:

- `AbstractCacheEntry::m_isSpm` as the single source of truth for physical SPM ownership.
- L1 `Entry` fields for `SrcAddr`, `DstSPMAddr`, `SPMSet`, `SPMWay`, and `Len` for debug/routing.
- Optional `SpmDirty` if `SPMST` dirtiness needs to be tracked separately from normal coherent `Dirty`.

Use `CacheMemory::allocateSPMSlot` as the base helper, with guide-specific validation:

- assert decoded `SPMSet` is in `0..1023`;
- assert decoded `SPMWay` is within the 8-way guide geometry and configured SPM-way count;
- assert the encoded slot maps to the intended private L1 set, or bypass normal address mapping and select `(set, way)` explicitly;
- panic if the target slot already holds SPM `X`;
- migrate a coherent non-SPM occupant to a free non-SPM way;
- rely on the software contract that at least two non-SPM ways are available.

Normal `cacheAvail`, `allocate`, `deallocate`, and `cacheProbe` paths must skip or reject SPM entries.

## Operation Flows

### `SPMCP_fetch`

1. L0 receives `SPMCP_fetch` with coherent source and encoded destination.
2. L0 forwards to L1 keyed by coherent source address.
3. L1 handles source state:
   - `I/NP`: send `GETS_SILENT` to L2/home.
   - `S`: send `PUTS`.
   - `E`: recall L0 if needed, then send `PUTE`.
   - `M`: recall L0 if needed, then send `PUTM + data`.
4. L2/home removes the source from coherence and returns data or `PUT_ACK`.
5. L1 claims the destination SPM slot and installs data as state `X`.
6. L1 deallocates the coherent source entry when required by the CC table.
7. L1 returns completion to L0/sequencer.

After completion, the coherent source address has no local coherent entry, and the data is reachable only through the encoded SPM slot.

### `SPMCP_install`

1. L0 forwards the encoded slot request to L1.
2. L1 decodes guide `SPMSet/SPMWay`.
3. L1 migrates any coherent occupant of that physical way to an available non-SPM way in the same set.
4. L1 marks the target way `X` and `isScratchpad=true`.
5. Reinstalling over existing `X` is rejected; software must issue `SPM_release` first.

### `SPMLD` and `SPMST`

`SPMLD`:

- Hit in local `X`: return SPM data.
- Outside `X`: return zero and complete.
- Missing/unclaimed slot: return zero, matching O3 ghost-load behavior.

`SPMST`:

- Hit in local `X`: update SPM bytes in place.
- Outside `X`: ignore and complete.
- Missing/unclaimed slot: ignore and complete.

### `SPMWB`

`SPMWB` is an SPM-to-main-memory writeback, not a normal coherent write to an active coherent line.

Recommended behavior:

- `SPMWB_read` reads local SPM data in `X`; outside `X`, return zero.
- `SPMWB_store` should only write to the intended memory-side/non-coherent destination path.
- If the destination address is still active in the coherence domain as a normal coherent line, return zeroes/complete benignly rather than issuing a coherent write or invalidation sequence.
- If a memory-side SPM writeback path is implemented, L1 sends `SPMWB_REQ` with data, waits in `XWB`, and returns to `X` on `SPMWB_ACK`.

This deliberately avoids using `SPMWB_store` as a coherent store instruction.

### `SPM_release`

1. L0 forwards release by encoded slot.
2. L1 verifies the local entry is `X` and `isScratchpad=true`.
3. L1 clears SPM metadata and marker bit.
4. L1 deallocates or invalidates the slot so the way becomes a normal replacement candidate.
5. Outside `X`, release is ignored and completed.

## Ordering and Speculation

The ISA guide requires `dsb sy` before overwriting reused SPM slots and after `SPMCP` before SPM loads. The protocol should also tolerate O3 ghost requests:

- Absent SPM loads return zero instead of asserting.
- Out-of-range or unclaimed SPM stores are ignored and acknowledged.
- `SPMCP_fetch` to an invalid source should follow normal memory semantics unless it is known to be squashed speculation.
- The O3 shadow table must compare instruction sequence numbers so future speculative `SPMCP` operations do not block older `SPMLD` operations to reused slots.

Protocol-level ordering:

- Maintain per-slot ordering for all SPM operations from a core.
- Maintain source-address ordering for `SPMCP_fetch` source removal.
- Do not let `SPMLD` observe a slot before matching `SPMCP_fetch` or `SPMCP_install` completion.

## Performance Requirements

The final model should expose realistic performance effects:

- SPM hits use private L1 tag/data latency and private-cache bank/port constraints.
- SPM ways reduce normal private L1 effective associativity and capacity.
- Normal replacement excludes SPM ways, increasing pressure on non-SPM ways.
- `SPMCP_fetch` from absent/private-miss sources pays coherence-home and memory latency.
- `SPMCP_fetch` from local `S/E/M` sources pays L0 recall/source-removal latency but no DRAM read when data is already local.
- `GETS_SILENT` from an owner in another private cache pays normal on-chip forwarding latency.
- `SPMWB_store` does not model a coherent write path; memory-side SPM writeback should be counted separately.
- Guide mapping conflicts across the same SPM sets/ways should be visible in stats.

Add stats:

- `spm.installs`, `spm.fetches`, `spm.loads`, `spm.stores`, `spm.writebacks`, `spm.releases`.
- `spm.load_hits`, `spm.load_zero_returns`, `spm.store_ignored`.
- `spm.slot_migrations`, `spm.reinstall_rejects`.
- `spm.replacement_blocks_due_spm`.
- `spm.bytes_read`, `spm.bytes_written`, `spm.bytes_copied`.
- Per-controller latency histograms for fetch, install, load, store, writeback, and release.

## Implementation Plan

1. Add the `MESI_Three_Level_SPM` protocol fork and build options.
2. Move SPM message enum extensions into `MESI_Three_Level_SPM-msg.sm`.
3. Extend L0 to classify SPM processor requests and forward them to L1.
4. Port the working two-level SPM L1 state-machine behavior into `MESI_Three_Level_SPM-L1cache.sm`, accounting for L0 recall.
5. Keep `CacheMemory::allocateSPMSlot`, migration, and replacement guards, but validate against guide set/way mapping.
6. Implement `GETS_SILENT`, `PUTS`, `PUTM`, `PUTE`, `PUT_ACK`, and optional `SPMWB_REQ/SPMWB_ACK` at L2 as the practical coherence home.
7. Keep Directory memory-side and avoid encoded SPM-slot ownership there.
8. Wire response routing back through L2/L1/L0 to the sequencer.
9. Add validation hooks and stats.
10. Build and run a simple non-SPM Ruby SE workload.
11. Run directed SPM simulations for every non-empty CC and Dir CSV row.
12. Run the GEMM/SPM benchmark configuration from `SPM_GEM5_GUIDE.md` and compare traffic/latency counters against expected SPM behavior.

## Validation Matrix

Minimum directed tests:

- `SPMCP_fetch` source absent: `GETS_SILENT`, no sharer added, SPM slot filled.
- `SPMCP_fetch` source `S`: `PUTS`, sharer removed, `PUT_ACK`, SPM slot filled.
- `SPMCP_fetch` source `E`: `PUTE`, owner cleared, `PUT_ACK`, SPM slot filled.
- `SPMCP_fetch` source `M`: `PUTM + data`, memory/home updated as needed, `PUT_ACK`, SPM slot filled.
- `SPMLD` hit: returns SPM data.
- `SPMLD` miss/wrong slot: returns zero.
- `SPMST` hit: updates only SPM data.
- `SPMST` miss/wrong slot: ignored and completed.
- `SPMWB_store` to an active coherent destination: returns zeroes/benign completion.
- `SPM_release`: slot becomes normal replacement candidate.
- Normal L1 replacement never selects SPM ways.
- `SPMCP_install` over a coherent occupant migrates that occupant without coherence messages.
- `SPMCP_install` over existing SPM is rejected.
- Forwarded `GETS/GETX/INV` for a coherent source in source-removal transient follows the CC table.
- `GETS_SILENT` in owner state routes owner data to SPM requester without adding that requester as sharer.

Formal/model checks:

- Extend `verification/rumur/cacheflex_spm_2core.m` with a gem5-three-level private L1 set/way model plus L2 home metadata.
- Prove an SPM slot is never a home sharer or owner.
- Prove normal private-L1 replacement cannot evict SPM ways.
- Prove local migration preserves coherent address/state/data identity and leaves home metadata unchanged.
- Prove two cores cannot both own the same private encoded SPM slot unless software explicitly partitions ownership.

## Final Open Question

For implementation, the remaining question is whether you want the Dir CSV behavior placed in `L2Cache` as the on-chip coherence home, which best matches the stock `MESI_Three_Level` performance model, or literally in `Directory`, which is simpler to map to the CSV name but less realistic for on-chip ownership transitions.
