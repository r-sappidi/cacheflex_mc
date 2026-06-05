# Ruby SPM Coherency Implementation Plan

## Summary

Implement CacheFlex SPM support as a clean extension of `MESI_Two_Level`, using
`SPM_GEM5_GUIDE.md`, `spm_coherency_CC.csv`, and `spm_coherency_dir.csv` as the
source of truth. Ignore the existing `MESI_Two_Level_SPML1.sm` and
`MESI_Two_Level_SPM-msg.sm` files entirely; implement the protocol directly in
the active MESI Two Level files or in newly named files that are explicitly
included by `MESI_Two_Level.slicc`.

The model will support:

- `SPMCP_fetch`: coherent source-address copy into SPM, removing the source line
  from the coherence domain.
- `SPMCP_install`: local SPM-way claim/install at the destination set/way.
- `SPMWB`: write SPM data back to a coherent address through the directory.
- `SPMLD` / `SPMST`: local SPM reads and writes.
- `SPM_release`: drop SPM data and return the physical way to normal cache use.
- Correct directory behavior for `GetS_silent`, `PutS`, `PutM`, `PutE`, and
  `SPMWB_Req`.

## Key Changes

### 1. Protocol Integration

- Extend the active protocol include graph:
  - Keep `MESI_Two_Level.slicc` as the selected protocol.
  - Add SPM behavior to the active `MESI_Two_Level-msg.sm`,
    `MESI_Two_Level-L1cache.sm`, `MESI_Two_Level-L2cache.sm` if needed, and
    `MESI_Two_Level-dir.sm`.
  - Do not include or modify `MESI_Two_Level_SPML1.sm` or
    `MESI_Two_Level_SPM-msg.sm`.
- Add Ruby request types for processor-originated SPM operations:
  - `SPMCP_fetch`
  - `SPMCP_install`
  - `SPMWB_read`
  - `SPMWB_store`
  - `SPMLD`
  - `SPMST`
  - `SPM_release`
- Add coherence message types:
  - Request: `GETS_SILENT`, `PUTS`, `PUTM`, `PUTE`, `SPMWB_REQ`
  - Response: `SPMWB_ACK`, `PUT_ACK` if the existing `ACK`/`WB_ACK` split is
    not sufficient
- Fix enum naming consistently:
  - Use one spelling only, preferably `SPMWB_REQ`.
  - Do not introduce mixed spellings such as `SPMWB_Req`.

### 2. Address and Metadata Model

- Preserve both address spaces for copy operations:
  - Source address: normal coherent physical line address.
  - Destination SPM address: `(way_id << 16) | (set_index << 6) | byte_offset`.
- Extend Ruby request/message/TBE metadata so an SPM copy can retain:
  - `srcAddr`
  - `dstSpmAddr`
  - decoded `spmSet`
  - decoded `spmWay`
  - request length
  - buffered `DataBlock`
  - pending ack/data flags
- Decode destination SPM fields using the guide:
  - `set_index = bits [15:6]`
  - `way_id = bits [16+]` according to the implemented SPM way count
  - `byte_offset = bits [5:0]`
- Treat SPM destination addresses as local SPM slots, not coherent addresses.

### 3. L1 / Core-Side State Machine

- Add stable SPM state:
  - `X`: line/slot is allocated to SPM and invalid in the coherence domain.
- Add transient states matching the CC table:
  - `IX^D`: source absent; issued `GetS_silent`; waiting for data to route into
    SPM.
  - `SX^A`: source was `S`; sent `PutS`; waiting for `Put-Ack`.
  - `MX^A`: source was `M`; sent `PutM + data`; waiting for `Put-Ack`.
  - `EX^A`: source was `E`; sent `PutE`; waiting for `Put-Ack`.
  - `XWB`: SPM writeback in progress.
  - Existing MESI transients should retain their current behavior.
- Implement `SPMCP_fetch` from the coherent source address:
  - If source is `I`/not present: send `GETS_SILENT` to directory and enter
    `IX^D`.
  - If source is `S`: send `PutS`, enter `SX^A`, and install SPM only after
    `Put-Ack`.
  - If source is `M`: send `PutM + data`, enter `MX^A`, and install SPM only
    after `Put-Ack`.
  - If source is `E`: send `PutE`, enter `EX^A`, and install SPM only after
    `Put-Ack`.
  - On data from directory/owner in `IX^D`, route data to the SPM destination
    and install `X`; do not allocate a coherent sharer entry.
- Implement `SPMCP_install` at the destination SPM set/way:
  - Select/claim the physical way indicated by the SPM destination.
  - If that way contains a coherent line, lazily migrate it to an available
    non-SPM way in the same set.
  - This migration is local physical relocation only; do not send coherence
    eviction/writeback messages.
  - Assume software guarantees at least two non-SPM ways per set remain
    available.
- Implement SPM local operations:
  - `SPMLD` in `X`: return SPM data.
  - `SPMST` in `X`: update SPM data.
  - `SPMLD`/`SPMST` outside `X`: return zero or ignore/update-zero behavior
    according to the CC table and existing guide gotchas.
  - `SPM_release` in `X`: clear scratchpad bit/metadata and transition back to
    `I`.
- Implement `SPMWB`:
  - `SPMWB_read` reads the local SPM data in `X`.
  - `SPMWB_store` sends `SPMWB_REQ(addr, SPM_data)` to the directory and enters
    `XWB`.
  - `SPMWB_Ack` transitions `XWB -> X`.

### 4. Directory State Machine

- Extend directory states to match the Dir table:
  - Existing `I`, `S`, `E`, `M` behavior should be preserved or mapped from the
    current MESI implementation.
  - Add `SD` as the transient for owner-forwarded read data.
  - Add TBE metadata field `silent` for `GetS_silent`.
- Implement `GETS_SILENT`:
  - In `I`: send data to requester and do not set owner or sharer.
  - In `S`: send data to requester and do not modify sharer list.
  - In `E`/`M`: forward `GetS` to owner, make owner a sharer, do not add
    requester as sharer, clear owner, set `TBE.silent = true`, enter `SD`.
  - In `SD`: stall.
- Implement normal `GetS` alongside silent behavior:
  - Preserve existing sharer-add behavior for non-silent `GetS`.
  - In `SD`, returned data should add the original requester as sharer only when
    `TBE.silent == false`.
- Implement ownership-removal requests:
  - `PutS-NotLast`: remove requester from sharers and send `Put-Ack`.
  - `PutS-Last`: remove requester, send `Put-Ack`, transition to `I`.
  - `PutM + data from Owner`: copy data to memory, send `Put-Ack`, clear owner,
    transition to `I`.
  - `PutM from Non-Owner`: send `Put-Ack`, do not change owner.
  - `PutE from Owner`: send `Put-Ack`, clear owner, transition to `I`.
  - `PutE from Non-Owner`: send `Put-Ack`, do not change owner.
- Implement `SPMWB_REQ`:
  - In `I`: write data to memory/LLC backing store and send `SPMWB_ACK`.
  - In states where a coherent owner/sharer exists, reject with assertion or
    stall only if impossible by protocol; the intended SPM flow removes the
    source from coherence before SPM ownership.

### 5. L2 / SPM Storage Behavior

- Model SPM as repurposed L2 ways:
  - Add per-block or per-way metadata indicating scratchpad ownership.
  - Ensure replacement policy does not select SPM ways for normal coherent
    replacement.
  - Ensure `SPMCP_install` claims the selected/encoded SPM way and marks it
    unavailable for coherent allocation.
- Implement local migration for `SPMCP_install`:
  - If destination way has a valid coherent line, move tag/data/state metadata to
    a free non-SPM way in the same set.
  - Do not call normal replacement paths.
  - Do not emit `PutS`, `PutM`, `PutE`, or eviction notifications for this
    relocation.
  - Assert if no non-SPM way is available, because the guide states software
    guarantees capacity.
- For SPM accesses:
  - `SPMLD` reads by SPM set/way/offset.
  - `SPMST` writes by SPM set/way/offset.
  - `SPMCP_fetch` data installs into the decoded SPM destination.
  - Invalid or speculative SPM loads should return zero rather than asserting,
    matching the guide's O3 ghost-load note.

### 6. ISA / Sequencer Plumbing

- Add or wire packet/request recognition for SPM operations before Ruby:
  - `SPMCP` must create a Ruby request carrying both source and destination SPM
    address.
  - `SPMWB` must carry both coherent destination address and source SPM slot.
  - `SPMLD`/`SPMST` must be tagged as SPM-space operations.
  - DSB behavior remains the software ordering contract for SPMCP-to-SPMLD
    ordering.
- Extend `RubyRequest`, `Sequencer`, and any request conversion code minimally so
  Ruby sees distinct request types and metadata.
- Preserve existing m5 pseudo-op behavior:
  - Only route opcodes with `op >= 2` to SPM decode if using the guide's ISA
    approach.
  - Do not break `m5_work_begin`, `m5_work_end`, or stats collection.

## Test Plan

### Build Tests

- Run SLICC/codegen build for the affected protocol:

  ```bash
  scons build/ARM/gem5.opt -j$(nproc)
  ```

- Also run broad build if protocol edits affect shared Ruby files:

  ```bash
  scons build/ALL/gem5.opt -j$(nproc)
  ```

### Unit / Protocol Scenarios

- `SPMCP_fetch` source absent:
  - L1 sends `GETS_SILENT`.
  - Directory returns data without adding requester as sharer.
  - L1 installs SPM state `X`.
- `SPMCP_fetch` source in `S`:
  - L1 sends `PutS`.
  - Directory removes sharer and sends `Put-Ack`.
  - SPM data installs only after ack.
- `SPMCP_fetch` source in `M`:
  - L1 sends `PutM + data`.
  - Directory writes data back, clears owner, sends `Put-Ack`.
  - SPM installs `X`.
- `SPMCP_fetch` source in `E`:
  - L1 sends `PutE`.
  - Directory clears owner and sends `Put-Ack`.
  - SPM installs `X`.
- `GetS_silent` while directory is `M` or `E`:
  - Directory forwards to owner.
  - Returned data is routed to original SPM requester.
  - Requester is not added as sharer.
- `SPMCP_install` over a coherent line:
  - Existing coherent occupant is moved to a non-SPM way in the same set.
  - No coherence eviction or writeback message is generated.
- `SPMLD`/`SPMST`:
  - Reads from valid `X` return SPM contents.
  - Stores to valid `X` update only SPM contents.
  - Accesses to unallocated SPM slots return zero or ignore writes per table
    behavior.
- `SPMWB`:
  - SPM data is sent as `SPMWB_REQ`.
  - Directory writes data and returns `SPMWB_ACK`.
  - L1 transitions `XWB -> X`.

### Simulation Tests

- Run a small SE-mode ARM binary that performs:
  - SPMCP into one SPM slot.
  - DSB.
  - SPM load.
  - Optional SPM store.
  - SPM writeback.
  - Normal coherent load from the writeback address to verify data.
- Run a multi-core Ruby test:
  - Core 0 owns line in `M`, then performs `SPMCP_fetch`.
  - Core 1 subsequently issues normal `GetS`.
  - Verify directory no longer treats Core 0 as coherent owner after SPM fetch.
- Run a speculative/O3 smoke test:
  - Use `DerivO3CPU` and the guide's required DSB barriers.
  - Confirm invalid speculative SPM loads return zero and do not assert.

## Assumptions

- The implementation targets `MESI_Two_Level` Ruby, not a new protocol name,
  unless build isolation requires a fork such as `MESI_Two_Level_SPM`.
- Existing `MESI_Two_Level_SPML1.sm` and `MESI_Two_Level_SPM-msg.sm` are ignored
  completely.
- The CSV tables are authoritative for state transitions.
- The directory table describes the Ruby directory controller, not the shared
  L2/SPM-way controller.
- `SPMCP_install` is local L2 way management and must not be modeled as
  coherence eviction.
- Software guarantees each set keeps enough non-SPM ways available for lazy
  migration.
- DSB barriers remain required software-visible ordering between SPMCP and later
  SPM loads.
