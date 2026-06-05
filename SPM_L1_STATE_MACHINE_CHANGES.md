# CacheFlex SPM L1 State-Machine Changes

This note documents the changes made to the base `MESI_Two_Level-L1cache.sm`
state machine to create `MESI_Two_Level_SPML1.sm`.

The goal of the fork is to model CacheFlex SPM behavior in the core's private
cache/L1 controller for the two-level correctness testbed. The CSV
`spm_coherency_CC.csv` is treated as the source of truth for this controller.
The directory-side CSV is separate and is not implemented here.

Important model decision: an SPM line is represented as a Ruby cache entry
stored at an encoded SPM address and marked with `setScratchpad(true)`. The
stable SPM protocol state is `X`. Normal coherent source addresses are kept
separate from encoded SPM slot addresses.

## File Summary

Primary L1 fork:

```text
gem5/src/mem/ruby/protocol/MESI_Two_Level_SPML1.sm
```

Shared support touched by the L1 SPM work:

```text
gem5/src/mem/ruby/structures/CacheMemory.cc
gem5/src/mem/ruby/structures/CacheMemory.hh
gem5/src/mem/ruby/slicc_interface/AbstractCacheEntry.hh
gem5/src/mem/ruby/protocol/RubySlicc_Types.sm
gem5/build_opts/ARM_MESI_Two_Level_SPM
```

Protocol include graph:

```text
gem5/src/mem/ruby/protocol/MESI_Two_Level_SPM.slicc
gem5/src/mem/ruby/protocol/MESI_Two_Level_SPM-msg.sm
```

## 1. Added SPM States

The base L1 states `I/S/E/M` only describe coherent cache lines. The SPM fork
adds one stable SPM state and several SPM transients.

Annotated code:

```slicc
state_declaration(State, desc="Cache states") {
  I, AccessPermission:Invalid;
  S, AccessPermission:Read_Only;
  E, AccessPermission:Read_Only;
  M, AccessPermission:Read_Write;

  // Stable SPM state. The entry is locally readable/writable by SPM ops but
  // is outside the coherent source-address domain.
  X, AccessPermission:Read_Write, desc="private-cache SPM line";

  // Source absent locally. L1 issued GETS_SILENT on the coherent source
  // address and is waiting for data to install at DstSPMAddr.
  IX_D, AccessPermission:Busy;

  // Source existed in this L1 as S/E/M. L1 sent a Put* to remove this
  // controller from the coherence domain and waits for Put_Ack before
  // installing the destination SPM slot.
  SX_A, AccessPermission:Busy;
  MX_A, AccessPermission:Busy;
  EX_A, AccessPermission:Busy;

  // SPM writeback request is outstanding. The SPM slot remains allocated.
  XWB, AccessPermission:Busy;
}
```

Why this was needed:

The CC table has an explicit SPM state `X`, and the SPM fetch paths require
different waiting behavior depending on the coherent source state. `IX_D`
buffers returned data in the TBE because the source was absent locally.
`SX_A/MX_A/EX_A` keep the source cache entry resident until `Put_Ack`, then
copy from the resident source entry into the destination SPM slot.

## 2. Added SPM Events

The base L1 maps normal processor requests such as `Load`, `Ifetch`, and
`Store`. The SPM fork adds processor-originated SPM events and response events
needed by the SPM protocol.

Annotated code:

```slicc
enumeration(Event, desc="Cache events") {
  Load;
  Ifetch;
  Store;

  SPMCP_fetch;    // coherent source copy into SPM
  SPMCP_install;  // local destination slot claim
  SPMLD;          // local SPM read
  SPMST;          // local SPM write
  SPMWB_read;     // local SPM read for writeback sequencing
  SPMWB_store;    // send SPM data to coherent destination address
  SPM_release;    // drop SPM slot

  Put_Ack;        // ack for PUTS/PUTM/PUTE source removal
  SPMWB_Ack;      // ack for SPM writeback request
}
```

Why this was needed:

Each SPM operation has protocol-visible behavior in `spm_coherency_CC.csv`.
Using explicit events keeps the SPM rows separate from normal `Load` and
`Store`, which prevents accidental coherent behavior on encoded SPM addresses.

## 3. Added Dual-Address Metadata

SPM copy/writeback operations need two addresses:

```text
SrcAddr     normal coherent source or writeback address
DstSPMAddr  encoded SPM slot address
```

Annotated code:

```slicc
structure(Entry, interface="AbstractCacheEntry") {
  State CacheState;
  DataBlock DataBlk;
  bool Dirty;

  Addr SrcAddr;       // coherent source or writeback address
  Addr DstSPMAddr;    // encoded SPM slot address
  int SPMSet;         // decoded set index from DstSPMAddr
  int SPMWay;         // decoded way id from DstSPMAddr
  int Len;            // operation length
}

structure(TBE) {
  State TBEState;
  DataBlock DataBlk;

  Addr SrcAddr;
  Addr DstSPMAddr;
  int SPMSet;
  int SPMWay;
  int Len;
}
```

Why this was needed:

`SPMCP_fetch` dispatches on the coherent source address but installs into the
encoded SPM destination address. `SPMWB_store` dispatches on the SPM slot but
sends a coherent writeback address to the directory. Without both addresses in
the TBE/request, the L1 can route responses to the wrong cache key.

## 4. Request Type Mapping

The base L1 converts `RubyRequestType` into internal events. The SPM fork
extends that mapping.

Annotated code:

```slicc
Event mandatory_request_type_to_event(RubyRequestType type) {
  if (type == RubyRequestType:LD) {
    return Event:Load;
  } else if (type == RubyRequestType:IFETCH) {
    return Event:Ifetch;
  } else if (type == RubyRequestType:ST ||
             type == RubyRequestType:ATOMIC) {
    return Event:Store;
  } else if (type == RubyRequestType:SPMCP_fetch) {
    return Event:SPMCP_fetch;
  } else if (type == RubyRequestType:SPMCP_install) {
    return Event:SPMCP_install;
  } else if (type == RubyRequestType:SPMLD) {
    return Event:SPMLD;
  } else if (type == RubyRequestType:SPMST) {
    return Event:SPMST;
  } else if (type == RubyRequestType:SPMWB_read) {
    return Event:SPMWB_read;
  } else if (type == RubyRequestType:SPMWB_store) {
    return Event:SPMWB_store;
  } else if (type == RubyRequestType:SPM_release) {
    return Event:SPM_release;
  }
}
```

Why this was needed:

Ruby must preserve SPM operation identity until it reaches the L1 state machine.
Collapsing these into normal loads/stores would trigger normal coherence lookup
and allocation behavior, which is wrong for SPM slots.

## 5. Mandatory Queue Address Ownership

The base L1 uses `in_msg.LineAddress` for most mandatory requests. SPM requests
need explicit address ownership rules.

Annotated code:

```slicc
if (in_msg.Type == RubyRequestType:SPMCP_fetch) {
  // Source-side operation. State-machine key is the coherent source line.
  Addr src := makeLineAddress(in_msg.SrcAddr);
  Entry source_entry := getCacheEntry(src);
  trigger(Event:SPMCP_fetch, src, source_entry, TBEs[src]);

} else if (in_msg.Type == RubyRequestType:SPMCP_install) {
  // Destination-side operation. State-machine key is encoded SPM slot.
  Addr dst := makeLineAddress(in_msg.DstSPMAddr);
  Entry spm_entry := getL1DCacheEntry(dst);
  trigger(Event:SPMCP_install, dst, spm_entry, TBEs[dst]);

} else if (in_msg.Type == RubyRequestType:SPMLD ||
           in_msg.Type == RubyRequestType:SPMST ||
           in_msg.Type == RubyRequestType:SPMWB_read ||
           in_msg.Type == RubyRequestType:SPMWB_store ||
           in_msg.Type == RubyRequestType:SPM_release) {
  // Local SPM operations operate on the encoded SPM slot.
  Addr dst := makeLineAddress(in_msg.DstSPMAddr);
  Entry spm_entry := getL1DCacheEntry(dst);
  trigger(mandatory_request_type_to_event(in_msg.Type), dst,
          spm_entry, TBEs[dst]);
}
```

Why this was needed:

The CC table describes the source line becoming `X` at an abstract protocol
level. In the implemented address model, the source coherent line and the
destination SPM slot have different lookup keys. The source uses the normal
coherent address and the destination uses the encoded SPM address. The L1
therefore routes `SPMCP_fetch` by the source address until coherence removal or
data fetch completes, then installs data at the encoded SPM slot.

## 6. SPM Request Message Actions

The L1 emits new message types declared in `MESI_Two_Level_SPM-msg.sm`.

Annotated code:

```slicc
action(spm_issueGETSSilent) {
  // Source absent locally. Ask the directory/L2 for data without adding this
  // requester as a coherent sharer.
  out_msg.addr := tbe.SrcAddr;
  out_msg.SrcAddr := tbe.SrcAddr;
  out_msg.DstSPMAddr := tbe.DstSPMAddr;
  out_msg.Type := CoherenceRequestType:GETS_SILENT;
}

action(spm_issuePUTS) {
  // Source was S. Remove this L1 from the sharer set before installing SPM.
  out_msg.addr := tbe.SrcAddr;
  out_msg.Type := CoherenceRequestType:PUTS;
}

action(spm_issuePUTM) {
  // Source was M. Return dirty data to the directory before installing SPM.
  out_msg.addr := tbe.SrcAddr;
  out_msg.Type := CoherenceRequestType:PUTM;
  out_msg.DataBlk := cache_entry.DataBlk;
  out_msg.Dirty := true;
}

action(spm_issuePUTE) {
  // Source was E. Clear directory owner state before installing SPM.
  out_msg.addr := tbe.SrcAddr;
  out_msg.Type := CoherenceRequestType:PUTE;
}

action(spm_issueSPMWBReq) {
  // SPM writeback sends data to the coherent writeback address, while the ack
  // carries DstSPMAddr so it can route back to the X slot.
  out_msg.addr := cache_entry.SrcAddr;
  out_msg.DstSPMAddr := cache_entry.DstSPMAddr;
  out_msg.Type := CoherenceRequestType:SPMWB_REQ;
  out_msg.DataBlk := cache_entry.DataBlk;
}
```

Why this was needed:

SPM fetch removes the source line from coherence before installing data in
`X`. The exact release message depends on whether the source was absent, `S`,
`E`, or `M`. SPM writeback is the reverse direction: it writes SPM data to a
normal coherent address through the directory.

## 7. Local SPM Completion Actions

The base L1 uses normal load/store callbacks. SPM operations need explicit
callbacks, including zero-data completion outside `X`.

Annotated code:

```slicc
action(spm_readHit) {
  L1Dcache.setMRU(cache_entry);
  sequencer.readCallback(address, cache_entry.DataBlk);
}

action(spm_readZero) {
  spmZeroData.clear();
  sequencer.readCallback(address, spmZeroData);
}

action(spm_storeHit) {
  cache_entry.DataBlk := in_msg.WTData;
  cache_entry.Dirty := true;
  L1Dcache.setMRU(cache_entry);
  sequencer.writeCallback(address, cache_entry.DataBlk);
}

action(spm_storeIgnore) {
  spmZeroData.clear();
  sequencer.writeCallback(address, spmZeroData);
}
```

Why this was needed:

Every SPM request that pops the mandatory queue must complete through the
sequencer. The CC table requires reads outside `X` to return zero and stores
outside `X` to be ignored while still completing the CPU request.

## 8. SPM Installation Actions

SPM installation is split into two paths because the data source differs.

Annotated code:

```slicc
action(spm_installFromTBE) {
  // IX_D path: source was not resident locally, so response data was buffered
  // into tbe.DataBlk.
  Entry spm_entry := L1Dcache[tbe.DstSPMAddr];
  if (is_invalid(spm_entry)) {
    spm_entry := L1Dcache.allocateSPMSlot(tbe.DstSPMAddr,
                                          tbe.SPMSet,
                                          tbe.SPMWay,
                                          new Entry);
  }
  spm_entry.CacheState := State:X;
  spm_entry.DataBlk := tbe.DataBlk;
  spm_entry.setScratchpad(true);
}

action(spm_installFromCache) {
  // SX_A/MX_A/EX_A paths: source entry is still resident while waiting for
  // Put_Ack, so copy directly from cache_entry.DataBlk.
  Entry spm_entry := L1Dcache[tbe.DstSPMAddr];
  if (is_invalid(spm_entry)) {
    spm_entry := L1Dcache.allocateSPMSlot(tbe.DstSPMAddr,
                                          tbe.SPMSet,
                                          tbe.SPMWay,
                                          new Entry);
  }
  spm_entry.CacheState := State:X;
  spm_entry.DataBlk := cache_entry.DataBlk;
  spm_entry.setScratchpad(true);
}

action(spm_claimSlot) {
  // SPMCP_install claims an encoded slot directly. CacheMemory handles local
  // same-set migration in the current functional model.
  if (is_invalid(cache_entry)) {
    set_cache_entry(L1Dcache.allocateSPMSlot(address,
                                            in_msg.SPMSet,
                                            in_msg.SPMWay,
                                            new Entry));
  }
  cache_entry.CacheState := State:X;
  cache_entry.setScratchpad(true);
}
```

Why this was needed:

`SPMCP_fetch` and `SPMCP_install` are not the same operation. Fetch copies data
from a coherent source and removes that source from coherence. Install claims a
destination SPM slot locally. In the current functional model, destination slot
migration is immediate local bookkeeping in `CacheMemory::allocateSPMSlot`;
timing-accurate LRU non-SPM eviction/migration is deferred.

## 9. Transient Stall and Invalidation Rules

SPM transients must not interleave with new processor work on the same address.

Annotated code:

```slicc
transition({IX_D, SX_A, MX_A, EX_A, XWB},
           {Load, Ifetch, Store, L1_Replacement, Load_Linked,
            SPMCP_fetch, SPMCP_install, SPMWB_store, SPMWB_read,
            SPM_release, SPMLD, SPMST}) {
  z_stallAndWaitMandatoryQueue;
}

transition({IX_D, SX_A, MX_A, EX_A, XWB},
           {Fwd_GETS, Fwd_GETX, Fwd_GET_INSTR}) {
  z_stallAndWaitL1RequestQueue;
}

transition({SX_A, MX_A, EX_A}, Inv) {
  fi_sendInvAck;
  l_popRequestQueue;
}

transition({IX_D, XWB}, Inv) {
  z_stallAndWaitL1RequestQueue;
}
```

Why this was needed:

`SX_A/MX_A/EX_A` still hold the coherent source line until the directory
confirms source removal. `IX_D` is an absent-source path, so it cannot satisfy
an invalidation with local data. `XWB` is not a normal coherent owner state.

## 10. SPM Fetch Transitions

The core CC-table SPM fetch paths were added from `I/S/E/M`.

Annotated code:

```slicc
transition({NP,I}, SPMCP_fetch, IX_D) {
  spm_allocateTBE;
  spm_recordRequestMetadata;
  spm_issueGETSSilent;
  k_popMandatoryQueue;
}

transition(S, SPMCP_fetch, SX_A) {
  i_allocateTBE;
  spm_recordRequestMetadata;
  spm_issuePUTS;
  k_popMandatoryQueue;
}

transition(E, SPMCP_fetch, EX_A) {
  i_allocateTBE;
  spm_recordRequestMetadata;
  spm_issuePUTE;
  k_popMandatoryQueue;
}

transition(M, SPMCP_fetch, MX_A) {
  i_allocateTBE;
  spm_recordRequestMetadata;
  spm_issuePUTM;
  k_popMandatoryQueue;
}
```

Why this was needed:

The source line must be removed from the coherent domain before the destination
SPM slot becomes valid. The absent-source path uses `GETS_SILENT` and never
adds the requester as a sharer. Resident source paths send `PUTS`, `PUTE`, or
`PUTM` and wait for `Put_Ack`.

## 11. SPM Fetch Completion

Fetch completion differs based on where the data comes from. The current L1
fork uses an immediate functional install after coherence completion; detailed
timing for local destination-way eviction/migration is deferred until the base
protocol is running end to end.

Annotated code:

```slicc
transition(IX_D, {Data_all_Acks, Data_Exclusive}, X) {
  // Directory or memory data path.
  spm_bufferResponseData;
  spm_installFromTBE;
  spm_fetchCompleteFromTBE;
  s_deallocateTBE;
  o_popIncomingResponseQueue;
  kd_wakeUpDependents;
}

transition(IX_D, DataS_fromL1, X) {
  // Owner L1 data path for GETS_SILENT when directory was E/M.
  // Do not send normal GETS unblock; silent request must not become a sharer.
  spm_bufferResponseData;
  spm_installFromTBE;
  spm_fetchCompleteFromTBE;
  s_deallocateTBE;
  o_popIncomingResponseQueue;
  kd_wakeUpDependents;
}

transition({SX_A, MX_A, EX_A}, Put_Ack, X) {
  // Source line is still resident locally, so copy from cache_entry.
  spm_installFromCache;
  spm_fetchCompleteFromTBE;
  ff_deallocateL1CacheBlock;
  s_deallocateTBE;
  o_popIncomingResponseQueue;
  kd_wakeUpDependents;
}
```

Why this was needed:

`IX_D` has no source entry and must install from response data buffered in the
TBE. `SX_A/MX_A/EX_A` still have the original source entry and install from the
cache entry only after the directory acknowledges source removal. This is a
functional model: it preserves the correct ownership outcome, but it does not
yet model the timing of evicting the LRU non-SPM way or locally moving the
destination-way occupant.

## 12. Local `X` State Behavior

The stable SPM state implements local SPM access, writeback, release, and
coherence isolation.

Annotated code:

```slicc
transition(X, {SPMLD, SPMWB_read}, X) {
  spm_readHit;
  k_popMandatoryQueue;
}

transition(X, SPMST, X) {
  spm_storeHit;
  k_popMandatoryQueue;
}

transition(X, SPMWB_store, XWB) {
  spm_issueSPMWBReq;
  k_popMandatoryQueue;
}

transition(XWB, SPMWB_Ack, X) {
  spm_writebackCompleteFromCache;
  o_popIncomingResponseQueue;
  kd_wakeUpDependents;
}

transition(X, SPM_release, I) {
  spm_writeCompleteFromCache;
  spm_releaseSlot;
  ff_deallocateL1CacheBlock;
  k_popMandatoryQueue;
}

transition(X, {Fwd_GETS, Fwd_GETX, Fwd_GET_INSTR, Inv}) {
  z_stallAndWaitL1RequestQueue;
}
```

Why this was needed:

SPM data is locally accessible but outside the coherent address space. Local
SPM reads/stores operate on the `X` entry. Coherent forwarded requests are
stalled because a coherent requester should not observe the SPM slot as a
coherent line.

## 13. Non-`X` SPM Operation Behavior

The CC table defines SPM operations outside `X` as zero/ignore/no-op behavior.

Annotated code:

```slicc
transition({NP,I,S,E,M}, {SPMLD, SPMWB_read}) {
  spm_readZero;
  k_popMandatoryQueue;
}

transition({NP,I,S,E,M}, {SPMST, SPMWB_store}) {
  spm_storeIgnore;
  k_popMandatoryQueue;
}

transition({NP,I,S,E,M}, SPM_release) {
  spm_writeCompleteZero;
  k_popMandatoryQueue;
}
```

Why this was needed:

Speculative or invalid SPM accesses should not assert or accidentally allocate
coherent lines. They complete deterministically according to the CC table.

## 14. Replacement Protection for `X`

The state machine has a guard for the case where replacement incorrectly
targets an SPM entry.

Annotated code:

```slicc
action(spm_errorReplacementSelected) {
  error("SPM entry selected by normal L1 replacement path");
}

transition(X, {L1_Replacement, PF_L1_Replacement}) {
  spm_errorReplacementSelected;
}
```

Why this was needed:

`CacheMemory::cacheProbe()` is responsible for excluding scratchpad entries from
normal replacement. If the generated L1 state machine ever receives
`L1_Replacement` or `PF_L1_Replacement` in state `X`, that indicates a bug in
the replacement/probe path. The explicit transition makes the failure
diagnostic instead of relying on an implicit unexpected-event error.

## 15. CacheMemory Support for SPM Slots

The L1 state machine calls into `CacheMemory` for destination slot allocation.
The shared cache structure was extended so an SPM slot behaves like a reserved
way for normal coherent traffic.

Annotated code:

```cpp
AbstractCacheEntry*
CacheMemory::allocateSPMSlot(Addr address, int spm_set, int spm_way,
                             AbstractCacheEntry *entry)
{
    int cacheSet = addressToCacheSet(address);
    panic_if(cacheSet != spm_set, ...);
    panic_if(spm_way >= m_cache_assoc, ...);

    migrateOrClearSPMSlot(cacheSet, spm_way);

    m_cache[cacheSet][spm_way] = entry;
    entry->m_Address = address;
    entry->setScratchpad(true);
    m_tag_index[address] = spm_way;
    return entry;
}
```

Replacement protection:

```cpp
bool
CacheMemory::cacheAvail(Addr address) const
{
    for (int way = 0; way < m_cache_assoc; way++) {
        AbstractCacheEntry *entry = m_cache[cacheSet][way];
        if (entry != nullptr && entry->isScratchpad()) {
            continue;
        }
        ...
    }
}

Addr
CacheMemory::cacheProbe(Addr address) const
{
    for (int way = 0; way < m_cache_assoc; way++) {
        if (m_cache[cacheSet][way]->isScratchpad()) {
            continue;
        }
        candidates.push_back(...);
    }
    ...
}
```

Why this was needed:

In real hardware, an SPM bit can make a way invisible to coherent tag lookup
and replacement. In gem5, SPM and coherent entries share the same `CacheMemory`
container, so the invisibility rule has to be modeled explicitly.

Current limitation:

`migrateOrClearSPMSlot()` is functional-only. It moves the destination-way
occupant to a free/non-present non-SPM way. It does not yet model the intended
future behavior of selecting the LRU non-SPM way, evicting that line normally,
then relocating the destination-way occupant.

## 16. SLICC Visibility for Scratchpad Metadata

The C++ `AbstractCacheEntry` has the scratchpad bit, but SLICC also needs to
know those methods exist.

Annotated code:

```slicc
structure(AbstractCacheEntry, primitive="yes", external="yes") {
  void changePermission(AccessPermission);
  void setScratchpad(bool);
  bool isScratchpad();
}
```

Why this was needed:

The L1 SLICC actions call `cache_entry.setScratchpad(true/false)` during SPM
install/release. Without these declarations, SLICC code generation rejects the
method calls even though the C++ methods exist.

## 17. Build Option Fix

The SPM build option was updated so it actually builds Ruby/SLICC and includes
the ARM ISA. In gem5 25.1, the old `ISA=ARM` setting is not enough; the build
metadata reports `ISA.NULL` unless `BUILD_ISA` and `USE_ARM_ISA` are set.

Annotated code:

```text
BUILD_ISA=y
USE_ARM_ISA=y
RUBY=y
PROTOCOL="MESI_Two_Level_SPM"
RUBY_PROTOCOL_MESI_Two_Level_SPM=y
```

Why this was needed:

Without `RUBY=y`, `scons build/ARM_MESI_Two_Level_SPM/gem5.opt` could appear
successful while not compiling the Ruby protocol or running SLICC generation.
Without `BUILD_ISA=y` and `USE_ARM_ISA=y`, the binary links but cannot run ARM
SE workloads; `configs/deprecated/example/se.py` fails with `ISA.NULL`.

## Current Validation Status

Completed:

```bash
scons defconfig build/ARM_MESI_Two_Level_SPM build_opts/ARM_MESI_Two_Level_SPM
scons build/ARM_MESI_Two_Level_SPM/mem/ruby/protocol/MESI_Two_Level_SPM/L1Cache_Controller.cc -j$(nproc)
scons build/ARM_MESI_Two_Level_SPM/gem5.opt -j$(nproc)
./build/ARM_MESI_Two_Level_SPM/gem5.opt -d m5out/spm-ruby-hello \
  configs/deprecated/example/se.py --ruby --cpu-type=TimingSimpleCPU \
  --num-cpus=1 -c tests/test-progs/hello/bin/arm/linux/hello
```

The L1 controller generation, full C++ compile/link, and a basic non-SPM Ruby
SE smoke test passed on May 26, 2026. The smoke test printed `Hello world!` and
gem5 exited normally at tick `28872000`; the simulated process reported exit
code 13, so this should be treated as a Ruby/controller smoke pass rather than
a clean guest-exit correctness test.

## Remaining L1-Side Work

The major L1 protocol behavior through C12 is implemented. Remaining L1-local
work is:

1. Add C14 debug/assertion hooks.
2. Run broader focused simulations after directory-side SPM support lands.

Directory-side SPM support is separate and still required before SPM-specific
simulations can pass end to end.
