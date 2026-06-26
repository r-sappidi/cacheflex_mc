# gem5/Ruby SPM Implementation Writeup

This document describes the gem5/Ruby implementation work needed to integrate CacheFlex-style SPM support with the existing `cacheflex_micro` CPU-side SPM implementation. It intentionally focuses on the Ruby memory-system changes rather than the CPU/ISA/LSQ machinery that already exists in `cacheflex_micro`.

The multicore CacheFlex implementation extends gem5 Ruby so the existing `cacheflex_micro` SPM ISA/runtime path can run under a coherent multicore memory hierarchy. The key goal was not to reimplement the CPU-side SPM instruction machinery, but to make those SPM requests meaningful inside Ruby: decode them into Ruby request types, route them through the private cache hierarchy, map encoded SPM addresses onto private-L2 cache ways, preserve coherence for source data, and isolate SPM-reserved storage from normal cache replacement.

The implementation is centered around a new Ruby protocol variant:

```text
MESI_Three_Level_SPM
```

This protocol is derived from gem5's existing `MESI_Three_Level` Ruby hierarchy, but adds explicit SPM request types, L0/L1 message paths, private-L1/L2 SPM slot management, silent coherent fetch support, SPM writeback support, and cache-way reservation/migration logic.

In the current naming, Ruby's hierarchy is:

```text
Ruby L0  -> private L1I/L1D
Ruby L1D -> private unified L2 / SPM array
Ruby L2  -> shared/banked LLC level
```

Most SPM logic therefore lives in the Ruby "L1" controller/cache, because that controller represents the private unified L2 where SPM ways are carved out.

Relevant files:

- `gem5/configs/ruby/MESI_Three_Level_SPM.py`
- `gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-L0cache.sm`
- `gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-L1cache.sm`
- `gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-L2cache.sm`
- `gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-dir.sm`
- `gem5/src/mem/ruby/protocol/RubySlicc_Exports.sm`
- `gem5/src/mem/ruby/protocol/RubySlicc_Types.sm`
- `gem5/src/mem/ruby/system/Sequencer.cc`
- `gem5/src/mem/ruby/system/RubyPort.cc`
- `gem5/src/mem/ruby/structures/CacheMemory.cc`
- `gem5/src/mem/ruby/structures/CacheMemory.hh`
- `gem5/src/mem/ruby/structures/RubyCache.py`
- `gem5/src/mem/ruby/slicc_interface/AbstractCacheEntry.hh`

## High-level design

The design treats SPM as a private, software-managed allocation of ways in the private Ruby L1D controller, i.e. the modeled private L2. An SPM slot is physically stored inside the same `CacheMemory` data array as normal cache lines, but it is marked as scratchpad and excluded from normal coherent cache replacement.

The implementation has four main design decisions:

1. SPM storage is implemented by reserving low-numbered ways of the private unified cache.
2. SPM accesses use explicit Ruby request types instead of pretending to be normal loads/stores.
3. SPM copy-in from memory/coherent cache uses a coherence-aware "silent read" path.
4. SPM data itself is thread-private and not exposed as normal coherent cache state.

Normal cache lines remain coherent through the ordinary MESI protocol. SPM slots are explicit software-managed private data.

## 1. Protocol creation and configuration

A separate protocol was added:

```text
MESI_Three_Level_SPM
```

The config file `gem5/configs/ruby/MESI_Three_Level_SPM.py` wraps the normal `MESI_Three_Level` configuration. Rather than duplicating the whole Python topology setup, it temporarily substitutes the SPM protocol controller classes:

```text
MESI_Three_Level_SPM_L0Cache_Controller
MESI_Three_Level_SPM_L1Cache_Controller
MESI_Three_Level_SPM_L2Cache_Controller
MESI_Three_Level_SPM_DMA_Controller
```

Then it calls the existing `MESI_Three_Level.create_system()`.

This was a pragmatic integration choice. The cache topology, network wiring, directory setup, CPU sequencer creation, and mesh/cluster handling are inherited from the existing stable protocol configuration. The SPM-specific protocol behavior is isolated in SLICC controller files.

The build target is:

```text
build/ARM_MESI_Three_Level_SPM/gem5.opt
```

The environment wrapper points to that binary:

```bash
GEM5_SPM=$GEM5_ROOT/build/ARM_MESI_Three_Level_SPM/gem5.opt
```

## 2. SPM configuration knob: reserved ways

The main user-visible Ruby knob is:

```bash
--l1d_num_spm_ways=N
```

This is defined in the base `MESI_Three_Level.py` config and passed into the private unified cache:

```python
l1_cache = L1Cache(
    size=options.l1d_size,
    assoc=options.l1d_assoc,
    start_index_bit=block_size_bits,
    is_icache=False,
    num_spm_ways=options.l1d_num_spm_ways,
)
```

The corresponding Ruby cache parameter is:

```python
num_spm_ways = Param.Int(
    0,
    "number of low ways [0, num_spm_ways) statically reserved for SPM; "
    "coherent data is confined to the remaining ways. 0 disables reservation"
)
```

The design reserves low-numbered ways:

```text
ways [0, num_spm_ways)      -> SPM-reserved ways
ways [num_spm_ways, assoc)  -> normal coherent cache ways
```

For the current experiments, the typical setting is:

```bash
--l1d_size=512kB
--l1d_assoc=8
--l1d_num_spm_ways=4
```

So four of the eight private-L2 ways are available for SPM placement.

A small replacement-policy detail was also handled: when SPM ways are reserved, the config switches that cache to LRU instead of TreePLRU. TreePLRU assumes the full associativity is a single replacement domain, but with SPM reservation the coherent replacement candidates become a restricted subset of the ways. LRU scans the explicit candidate list and is safer for this setup.

## 3. SPM address encoding

The existing `cacheflex_micro` SPM code uses encoded SPM addresses. Ruby decodes those addresses into:

```text
set index
way id
offset
```

The encoding used by the benchmark/runtime is:

```cpp
spm_addr = (way << 16) | (set << 6) | offset;
```

So:

```text
bits [5:0]    -> byte offset within 64B cache line
bits [15:6]   -> SPM/cache set index
bits [18:16]  -> SPM way id
```

This matches a 512KB, 8-way, 64B-line private cache:

```text
512KB / 8 ways / 64B = 1024 sets
```

That is why the current configuration uses a 512KB private L2/SPM. The SPM set field has 10 bits, exactly matching 1024 sets.

The Ruby sequencer masks the way field to the 8-way guide geometry:

```cpp
msg->m_SPMWay = pkt->req->getSPMWay() & 0x7;
```

This was done so software can place the encoded SPM virtual/physical window safely while Ruby still extracts the architectural way index expected by the SPM layout.

## 4. Ruby request type integration

The CPU-side `cacheflex_micro` path already marks requests as SPM operations. The Ruby integration adds corresponding request types to Ruby's protocol-visible request enum:

```text
SPMCP_fetch
SPMCP_install
SPMLD
SPMST
SPMWB_read
SPMWB_store
SPM_release
```

Their roles are:

```text
SPMCP_fetch    copy a coherent source cache line into an SPM slot
SPMCP_install  claim/install an SPM slot directly
SPMLD          read from an SPM slot
SPMST          write/update an SPM slot
SPMWB_read     read SPM data for software writeback
SPMWB_store    write SPM data back to a coherent destination
SPM_release    release an SPM slot
```

Ruby's utility classification was updated so these requests are treated correctly as read-like or write-like:

```text
isWriteRequest:
  SPMST
  SPMWB_store
  SPMCP_fetch
  SPMCP_install
  SPM_release

isDataReadRequest:
  SPMLD
  SPMWB_read
```

This matters because Ruby uses those helpers for sequencing, callbacks, and stats classification.

## 5. Metadata added to RubyRequest

SPM requests need more information than a normal load/store. A normal memory request has one address. SPM operations often have two logical addresses:

```text
source coherent address
destination encoded SPM slot address
```

So `RubyRequest` was extended with:

```text
SrcAddr
DstSPMAddr
SPMSet
SPMWay
```

These fields flow from the CPU request into the Ruby sequencer, then into SLICC messages between L0, private L1, LLC, and directory.

## 6. Sequencer routing

The Ruby sequencer is the main bridge between CPU packets and Ruby protocol messages.

The key function is:

```cpp
rubyRequestAddress(PacketPtr pkt, RubyRequestType type)
```

For normal requests, Ruby uses `pkt->getAddr()`.

For SPM requests, the address used to key the Ruby transaction depends on operation type:

```text
SPMCP_fetch:
  key by translated physical source address

SPMWB_store:
  key by coherent writeback destination/source address

SPMCP_install, SPMLD, SPMST, SPMWB_read, SPM_release:
  key by encoded SPM destination slot address
```

This distinction is important. `SPMCP_fetch` must participate in coherence for the source line, so it is keyed by the physical source address. SPM slot operations are private scratchpad operations, so they are keyed by the SPM slot address.

The sequencer recognizes CPU-side request flags:

```cpp
pkt->req->isSPMCPFetch()
pkt->req->isSPMCPInstall()
pkt->req->isSPMLD()
pkt->req->isSPMST()
pkt->req->isSPMWBRead()
pkt->req->isSPMWBStore()
pkt->req->isSPMRelease()
```

and maps them to Ruby request types.

Then, during `issueRequest`, the sequencer fills the SPM metadata:

```cpp
msg->m_SrcAddr
msg->m_DstSPMAddr
msg->m_SPMSet
msg->m_SPMWay
```

Special case: for `SPMCP_fetch`, the true coherent source is the translated packet address. The metadata source address captured earlier may still be virtual, so Ruby intentionally uses:

```cpp
pkt->getAddr()
```

as the coherence address. This avoids accidentally using a pre-translation virtual address to index Ruby coherence state.

## 7. RubyPort changes

`RubyPort` normally checks whether a request address belongs to physical memory and may route non-memory addresses to PIO. SPM encoded addresses do not necessarily look like ordinary physical memory addresses, so RubyPort was adjusted to allow SPM requests through Ruby instead of treating them as PIO or rejecting them.

The relevant checks were changed to exempt SPM requests:

```cpp
if (!pkt->req->isSPMRequest() && !isPhysMemAddress(pkt)) {
    ...
}
```

The hit callback also allows SPM requests:

```cpp
assert(pkt->req->isSPMRequest() ||
       system->isMemAddr(pkt->getAddr()) ||
       ...);
```

And SPM requests are prevented from accessing gem5's backing physical memory in `hitCallback`:

```cpp
if (pkt->req->isSPMRequest() || pkt->isFlush() || ...) {
    accessPhysMem = false;
}
```

That is required because an SPM load/store should complete from the Ruby SPM slot, not from gem5's backing memory array at the encoded SPM address.

This is one of the key integration fixes: SPM addresses are meaningful to the SPM/Ruby protocol, not necessarily to physical DRAM.

## 8. L0 controller integration

In the Ruby hierarchy, the L0 controller sits closest to the CPU. It receives mandatory queue requests from the sequencer.

The SPM L0 controller was extended to recognize the new Ruby request types and forward them to the private L1 controller as SPM-specific coherence classes:

```text
RubyRequestType::SPMCP_fetch    -> CoherenceClass::SPMCP_FETCH
RubyRequestType::SPMCP_install  -> CoherenceClass::SPMCP_INSTALL
RubyRequestType::SPMLD          -> CoherenceClass::SPM_LD
RubyRequestType::SPMST          -> CoherenceClass::SPM_ST
RubyRequestType::SPMWB_read     -> CoherenceClass::SPMWB_READ
RubyRequestType::SPMWB_store    -> CoherenceClass::SPMWB_STORE
RubyRequestType::SPM_release    -> CoherenceClass::SPM_RELEASE
```

The L0 does not own the SPM array. It acts as the CPU-facing request/response point:

```text
CPU/Sequencer -> L0 mandatory queue -> private L1/SPM
```

The L0 completes the original CPU request when it receives either:

```text
SPM_DATA
SPM_ACK
```

from private L1.

A dedicated SPM response path was added:

```text
spmBufferFromL1 / spmBufferToL0
```

The regular L1-to-L0 buffer still exists, but SPM responses use a dedicated ordered buffer to model the separate SPM return path and avoid mixing SPM data responses with normal coherence responses.

## 9. L1/private-L2 SPM controller behavior

The private Ruby L1 controller is where the SPM semantics are implemented. Again, this is effectively the private L2/SPM level in the modeled hierarchy.

The SPM protocol adds a new stable state:

```text
X
```

Meaning:

```text
Private scratchpad slot outside coherence domain
```

Normal MESI states remain:

```text
I, S, SS, E, EE, M, MM, ...
```

SPM slots use `X`, not `S/E/M`, because they are not coherent cache lines. They are software-managed private scratchpad entries.

The L1 controller handles:

```text
SPMCP_fetch
SPMCP_install
SPMLD
SPMST
SPMWB_read
SPMWB_store
SPM_release
```

### SPMCP_fetch

`SPMCP_fetch` copies data from a coherent source line into a private SPM slot.

Cases:

1. Source is absent locally:
   - Issue `GETS_SILENT` toward the L2/directory.
   - Receive data.
   - Install data into the encoded SPM slot.
   - Complete to L0 with an ack.
2. Source is present in clean shared/exclusive state:
   - Copy resident data into the SPM slot.
   - Depending on state, release or downgrade the coherent source as needed.
3. Source is modified in L0:
   - Recall data from L0.
   - Use the returned data to install the SPM slot.
4. Source is modified/exclusive in private L1:
   - Copy data into SPM.
   - Release/update source ownership through PUT-style messages.

The SPM fetch path has transient states such as:

```text
IX_D
SX_L0
MX_L0
EX_L0
SX_A
MX_A
EX_A
```

These distinguish whether the source was absent, resident, waiting on L0 recall, or waiting on a coherence-side release ack.

### GETS_SILENT

`GETS_SILENT` is a key addition.

A normal `GETS` would add the requester as a sharer in the coherence directory. That would be wrong for SPM copy-in if the destination is not a coherent cache line. The SPM only needs a data snapshot.

So the protocol adds:

```text
CoherenceRequestType::GETS_SILENT
```

Semantics:

```text
Fetch the data snapshot needed for SPM copy-in, but do not install the SPM requester as a coherent sharer/owner.
```

If another private cache owns the freshest data, it can respond with data, but the SPM destination remains outside the normal sharer set.

This is central to keeping SPM storage private and non-coherent while still obtaining correct source data.

### SPMCP_install

`SPMCP_install` directly claims an SPM slot. This is useful for software-managed initialization or cases where the slot is being explicitly created before being filled.

The private L1 allocates a scratchpad-marked cache entry at the encoded set/way and transitions it to `X`.

### SPMLD

`SPMLD` reads from an SPM slot.

If the entry is in state `X`, the controller reads the data from the SPM slot and returns `SPM_DATA` to L0.

If the slot is absent/non-SPM, the implementation returns zero data for the read-like path. This makes absent SPM reads deterministic in the current model rather than falling into normal memory.

### SPMST

`SPMST` updates an existing SPM slot. The private L1 writes the incoming data block into the scratchpad entry and returns `SPM_ACK`.

### SPMWB_read

`SPMWB_read` reads an SPM slot for software writeback. It is similar to `SPMLD` but classified separately so software/runtime can distinguish normal SPM consumption from writeback staging.

### SPMWB_store

`SPMWB_store` writes SPM data back to a coherent memory-side destination.

The private L1 sends:

```text
SPMWB_REQ
```

toward the home L2/directory. When the directory/memory side completes the writeback, it returns:

```text
SPMWB_ACK
```

Then the private L1 completes the CPU-side operation with `SPM_ACK`.

### SPM_release

`SPM_release` invalidates/releases an SPM slot. The entry leaves state `X` and returns to `I`.

Normal replacement is not allowed to pick an SPM line. SPM slots are released explicitly by the software/runtime path.

## 10. Directory and LLC-side protocol additions

The directory protocol was extended to support SPM copy and writeback without corrupting coherence state.

New request/response concepts include:

```text
GETS_SILENT
PUTS
PUTM
PUTE
SPMWB_REQ
PUT_ACK
SPMWB_ACK
```

Directory-side transient states include:

```text
SPM_IS
SPM_WB
SPM_PUT_WB
```

Their roles:

```text
SPM_IS:
  directory is fetching memory data for a silent SPM copy

SPM_WB:
  directory is processing SPM writeback data

SPM_PUT_WB:
  directory is handling source-release data from a modified SPM copy source
```

The directory behavior is:

- `GETS_SILENT` returns data without registering the requester as a sharer.
- `PUTS` / `PUTE` release clean shared/exclusive source state.
- `PUTM` releases modified source data and writes data back if needed.
- `SPMWB_REQ` writes SPM data to the coherent destination and returns `SPMWB_ACK`.

This allows SPM copy-in and writeback to interact with the memory hierarchy without making the SPM slot a coherent line.

## 11. Message format extensions

The SPM protocol extends Ruby message formats.

`RequestMsg` and `ResponseMsg` gained:

```text
SrcAddr
DstSPMAddr
Len
Dirty
```

`CoherenceMsg` between L0 and L1 gained:

```text
SrcAddr
DstSPMAddr
SPMSet
SPMWay
Len
Dirty
```

These fields carry the SPM operation metadata through the hierarchy.

The fields are needed because SPM operations often involve both:

```text
coherent source/destination address
encoded SPM slot address
```

A normal Ruby request only has one line address, so the metadata had to be explicit.

## 12. CacheMemory changes

The core storage integration is in `CacheMemory`.

### Scratchpad marker

`AbstractCacheEntry` was extended with a scratchpad marker:

```cpp
setScratchpad(bool)
isScratchpad()
```

This lets generic cache code identify SPM entries without downcasting to protocol-specific entries.

### Reserving ways

`CacheMemory` stores:

```cpp
m_num_spm_ways
```

and validates:

```cpp
0 <= num_spm_ways < associativity
```

Normal coherent allocation skips reserved SPM ways:

```cpp
for (int i = m_num_spm_ways; i < m_cache_assoc; i++)
```

This affects:

```text
cacheAvail()
allocate()
cacheProbe()
replacement victim selection
```

So normal cache fills and replacements cannot consume reserved SPM ways.

### allocateSPMSlot

SPM allocation uses:

```cpp
allocateSPMSlot(address, spm_set, spm_way, entry)
```

This:

1. Checks that the encoded address maps to the decoded SPM set.
2. Checks that the way is legal for the cache associativity.
3. Ensures the slot is not already present.
4. Clears or migrates the existing occupant.
5. Installs the new entry exactly at the requested set/way.
6. Marks it as scratchpad.

Unlike normal `allocate()`, this function does not search for any free way. The whole point is that software has explicitly selected the set and way.

### migrateOrClearSPMSlot

If the requested SPM slot already contains something, `migrateOrClearSPMSlot()` handles it.

Cases:

```text
empty slot:
  do nothing

invalid/not-present placeholder:
  delete it

live coherent line:
  relocate it to a free non-SPM way in the same set

existing SPM line:
  panic, because software tried to reinstall without release
```

The migration is purely an internal physical relocation within the cache set. Coherence state and data move with the entry. No coherence message is emitted because the logical cache line is still in the same private cache; only the physical way changes.

This was necessary because an SPM install should not incorrectly evict coherent data if the cache can move it into the coherent-way partition.

### readSPMData

`readSPMData(address, size)` reads SPM data out of one or more consecutive SPM slots.

It handles wide SPM reads that may span multiple 64B slots/ways. It starts at the encoded way and advances ways while preserving the same set.

It also checks that a wide read does not cross beyond the associativity or change sets.

This matters for SVE accesses where one vector operation may be wider than a single cache line depending on VL and element type.

## 13. SPM response latency and dedicated path

The L1 SLICC controller defines:

```text
spm_response_latency := 6
```

This models the private-L2 SPM array access plus return to Ruby L0.

SPM responses are sent over:

```text
spmBufferToL0
```

rather than only the normal coherence response buffer.

The motivation is to model the SPM path as a distinct private datapath from the normal coherent cache response path. It also avoids entangling SPM request completion with normal L1/L0 coherence invalidation and data response traffic.

## 14. Integration with cacheflex_micro toolchain

The `cacheflex_micro` side already had:

```text
SPM mnemonics / inline asm
custom encoding pass
SPM-aware request flags in the CPU/LSQ path
SPM copy/load/store/writeback/release instruction flow
```

The multicore repo reuses that path.

The build flow is:

1. Compile SPM benchmark source to assembly.
2. Run the SPM compiler/encoding pass.
3. Assemble/link the encoded assembly.
4. Run on the SPM Ruby gem5 binary.

For GEMM:

```bash
SPM_VL=16 bash benchmarks/gemm/build_acl_fp16_mc.sh
```

The build script includes `cacheflex_micro` headers:

```bash
-I ../cacheflex_micro/kernels/llama_bench_spm
```

and runs:

```bash
python3 "$SPM_COMPILER" input.s output_enc.s
```

So the CPU-visible SPM instructions come from `cacheflex_micro`. The new work here is that Ruby now understands the resulting request types and routes them through a multicore coherent hierarchy.

## 15. Runtime benchmark configuration

A representative run uses:

```bash
--ruby
--num-cpus=8
--num-dirs=8
--num-l2caches=8
--cpu-type=DerivO3CPU
--l0i_size=64kB
--l0d_size=64kB
--l0i_assoc=4
--l0d_assoc=4
--l1d_size=512kB
--l1d_assoc=8
--l1d_num_spm_ways=4
--l2_size=4MB
--l2_assoc=16
--enable-prefetch
```

In the current Cortex-A710-inspired multicore run, the core/cache config is:

```text
2.5GHz CPU/system clocks
5-wide fetch/decode/rename
4-wide commit
8-wide dispatch/issue/writeback
64KB 4-way private L1I/L1D
512KB 8-way private L2/SPM
4MB 16-way LLC bank
ROB128
IQ80
LQ32 / SQ48
SPMLQ32 / SPMSQ48
IntRF128 / FPRF192 / VecRF192 / PredRF64
```

The important SPM-specific Ruby knob is:

```bash
--l1d_num_spm_ways=4
```

Baseline runs use:

```bash
--l1d_num_spm_ways=0
```

SPM runs use:

```bash
--l1d_num_spm_ways=4
```

## 16. Why this design is coherent

The design keeps coherence correct by separating three concepts:

### Normal cache data

Normal loads/stores use normal MESI states and normal Ruby coherence.

### Coherent source/destination of SPM operations

SPM copy-in and writeback interact with the coherent hierarchy through explicit protocol operations:

```text
GETS_SILENT
PUTS / PUTM / PUTE
SPMWB_REQ
```

So source data is fetched correctly, and dirty source/writeback data is propagated correctly.

### SPM slot contents

The SPM slot itself is not a coherent cache line. It is marked scratchpad and stored in state `X`.

Normal coherence requests do not invalidate or probe SPM slots as sharers because SPM slots are not added to the directory sharer set.

That is the right model for software-managed private scratchpad storage.

## 17. Why GETS_SILENT was necessary

Without `GETS_SILENT`, SPM copy-in would have had to use a normal `GETS`.

That would cause the SPM-owning private cache to become a sharer of the source line even though the data was copied into non-coherent SPM storage. This would be semantically wrong:

- the SPM slot would look like coherent cached data;
- future invalidations could be expected to reach it;
- directory sharer state would over-approximate real coherent ownership;
- SPM would no longer be cleanly outside coherence.

`GETS_SILENT` fixes this by fetching a data snapshot without registering coherent ownership.

## 18. Why SPM slots use cache ways instead of a separate array object

The implementation models SPM as way-level cache reconfiguration rather than a completely separate gem5 object.

That gives several benefits:

1. It matches the CacheFlex architectural claim: part of the private cache is repurposed as SPM.
2. It naturally models capacity tradeoff: reserving SPM ways reduces coherent cache capacity.
3. It reuses Ruby cache timing/storage machinery.
4. It allows direct set/way placement corresponding to the encoded SPM address.
5. It makes normal replacement exclusion straightforward.

The cost is that `CacheMemory` needed explicit support for scratchpad entries and fixed-way allocation.

## 19. Important limitations and modeling assumptions

The current model is intended to capture the architectural behavior and performance effects of private-cache way reconfiguration. It does not attempt to model every physical detail of a final hardware SPM datapath.

Key assumptions:

- SPM is private to the core/thread using that private cache.
- SPM slots are explicitly managed by software/runtime.
- SPM contents are outside normal coherence after copy-in.
- SPM way reservation is static per run through `l1d_num_spm_ways`.
- Current encoded address geometry assumes the 512KB/8-way/64B private cache layout.
- Reinstalling an already-live SPM slot without release is treated as an error.
- SPM read of absent/non-SPM slot returns deterministic zero data in the modeled path.
- SPM response latency is modeled with a fixed SLICC latency parameter.

## 20. Summary of what was implemented

The Ruby work did the following:

- Added a new `MESI_Three_Level_SPM` protocol.
- Reused the existing `MESI_Three_Level` Python topology setup.
- Added SPM Ruby request types.
- Added SPM metadata fields to Ruby requests/messages.
- Modified the Ruby sequencer to classify and route SPM CPU requests.
- Modified RubyPort so encoded SPM addresses are not treated as PIO or backing-memory accesses.
- Added an L0 forwarding path for SPM operations.
- Added a dedicated L1-to-L0 SPM response buffer.
- Added private-L1/private-L2 SPM state `X`.
- Added SPM copy/load/store/writeback/release transitions.
- Added `GETS_SILENT` to fetch coherent source data without adding SPM as a sharer.
- Added PUT-style source release and SPM writeback protocol messages.
- Added directory handling for silent fetch, source release, and SPM writeback.
- Added `num_spm_ways` to Ruby cache objects.
- Prevented normal cache allocation/replacement from using reserved SPM ways.
- Added exact set/way SPM allocation through `allocateSPMSlot`.
- Added migration of coherent occupants out of a requested SPM way when possible.
- Added scratchpad marking to cache entries.
- Added SPM data read support across one or more slots/ways.
- Integrated the `cacheflex_micro` SPM compiler/runtime path with the Ruby protocol so the existing encoded SPM instructions execute correctly in multicore Ruby simulation.

The net result is that `cacheflex_micro`'s CPU-side SPM instructions now drive a Ruby-visible, coherent-system-aware private SPM implementation instead of only working in the original simpler/single-core `cacheflex_micro` setup.

## 21. Tunable knobs and implementation parameters

This section collects the main knobs that are particular to this SPM/Ruby implementation. Some are true architectural parameters, some are gem5 modeling parameters, and some are benchmark/runtime controls. They are separated because changing them has different implications.

### 21.1 Protocol and build knobs

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| Ruby protocol | `MESI_Three_Level_SPM` | gem5 build target | Selects the SPM-aware Ruby protocol instead of the vanilla `MESI_Three_Level` protocol. |
| gem5 binary | `build/ARM_MESI_Three_Level_SPM/gem5.opt` | `setup_spm_env.sh` | Ensures runs use the Ruby protocol containing SPM request types and transitions. |
| SPM compiler | `spm_tools/spm_compiler.py` | `setup_spm_env.sh`, benchmark build scripts | Translates CacheFlex SPM mnemonics into the custom encoded instruction stream expected by the CPU-side SPM implementation. |
| SPM kernel VL macro | `SPM_VL=16` for the current GEMM path | `benchmarks/gemm/build_acl_fp16_mc.sh` | Selects the `cacheflex_micro` SPM kernel variant at compile time. Must match the gem5 SVE vector length used in simulation. |

Changing the protocol or binary path changes whether SPM requests are understood by Ruby at all. Changing `SPM_VL` changes the compiled SPM kernel shape and must be kept consistent with `sve_vl_se`.

### 21.2 Ruby hierarchy knobs

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| `--num-cpus` | `8` | benchmark runner | Number of simulated cores/sequencers. |
| `--num-dirs` | `8` | benchmark runner | Number of Ruby directory controllers. Current runs use one directory per core. |
| `--num-l2caches` | `8` | benchmark runner | Number of Ruby LLC banks/controllers. Current runs use one bank per core. |
| `--topology` | `Mesh_XY` | benchmark runner | Ruby network topology. |
| `--mesh-rows` | `2` for 8-core runs | benchmark runner | Mesh shape control. With 8 nodes, this creates a 2-row mesh. |
| `--ruby` | enabled | benchmark runner | Selects Ruby instead of the classic memory system. Required for the SLICC protocol. |

These knobs control multicore topology and directory/LLC banking. They affect contention, message hop counts, and directory/cache-bank distribution. For publication-quality scaling studies, these should be swept separately from SPM kernel tile tuning.

### 21.3 Cache hierarchy and SPM storage knobs

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| `--cacheline_size` | `64` bytes | benchmark runner | Defines the line size used by Ruby and the SPM address encoding. |
| `--l0i_size` | `64kB` | benchmark runner | Private L1I size in the Ruby hierarchy. |
| `--l0d_size` | `64kB` | benchmark runner | Private L1D size in the Ruby hierarchy. |
| `--l0i_assoc` | `4` | benchmark runner | Private L1I associativity. |
| `--l0d_assoc` | `4` | benchmark runner | Private L1D associativity. |
| `--l1d_size` | `512kB` | benchmark runner | Private unified L2/SPM capacity. This is the cache whose ways are repurposed as SPM. |
| `--l1d_assoc` | `8` | benchmark runner | Private unified L2/SPM associativity. Required by the current 3-bit SPM way encoding. |
| `--l1d_num_spm_ways` | `0` for baseline, `4` for SPM | benchmark runner | Number of low-numbered private-L2 ways reserved for SPM slots. |
| `--l2_size` | `4MB` per Ruby L2 bank | benchmark runner | Banked LLC capacity behind private caches. |
| `--l2_assoc` | `16` | benchmark runner | LLC associativity. |
| Replacement policy for private L2/SPM | `LRU` when `l1d_num_spm_ways > 0` | `gem5/configs/ruby/MESI_Three_Level.py` | Avoids TreePLRU assumptions over the full associativity when coherent data is restricted to a subset of ways. |

The most important SPM knob is:

```bash
--l1d_num_spm_ways
```

The implementation reserves low-numbered ways:

```text
ways [0, l1d_num_spm_ways)      -> SPM
ways [l1d_num_spm_ways, assoc)  -> normal coherent cache
```

Increasing `l1d_num_spm_ways` gives software more SPM capacity but reduces normal coherent private-L2 capacity. Decreasing it does the opposite. A value of `0` disables static SPM reservation and is used for the non-SPM baseline.

The current GEMM setup uses four SPM ways in an eight-way private L2:

```text
4 SPM ways + 4 coherent ways
```

### 21.4 SPM address mapping knobs

| Field | Current mapping | Meaning |
|---|---:|---|
| offset | bits `[5:0]` | Byte offset within a 64B slot/cache line. |
| set | bits `[15:6]` | SPM/cache set index. |
| way | bits `[18:16]` | SPM way index. |

The software-visible address formula is:

```cpp
spm_addr = (way << 16) | (set << 6) | offset;
```

This implies:

```text
64B line size
1024 sets
8 encoded ways
```

That maps naturally to:

```text
512KB = 1024 sets * 8 ways * 64B
```

The private L2/SPM is therefore configured as `512kB`, `8`-way for the current experiments.

The sequencer masks the software-provided way field:

```cpp
msg->m_SPMWay = pkt->req->getSPMWay() & 0x7;
```

This preserves the 3-bit encoded SPM geometry even if software places the SPM address window at a higher physical/virtual base. If the private L2 size, associativity, or line size changes, this encoding must be revisited.

### 21.5 SLICC protocol latency knobs

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| `l1_request_latency` | `2` cycles | `MESI_Three_Level_SPM-L1cache.sm` | Latency for L1-to-network/control request actions. |
| `l1_response_latency` | `2` cycles | `MESI_Three_Level_SPM-L1cache.sm` | Latency for normal L1 response messages. |
| `spm_response_latency` | `6` cycles | `MESI_Three_Level_SPM-L1cache.sm` | Models private-L2 SPM array access plus return to Ruby L0. |
| `to_l2_latency` | `1` cycle | `MESI_Three_Level_SPM-L1cache.sm` | Local latency component for sending traffic toward L2. |

`spm_response_latency` is the key SPM datapath timing knob. Reducing it makes SPM loads/stores more aggressive; increasing it models a slower SPM access path. This parameter should be justified against the intended microarchitecture because it directly affects measured SPM speedups.

### 21.6 Core model knobs

The current multicore GEMM runner is configured to match the Cortex-A710-inspired CPU table used in the paper methodology.

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| CPU type | `DerivO3CPU` | benchmark runner | gem5 out-of-order core model. |
| `--cpu-clock` | `2.5GHz` | benchmark runner | Core clock. |
| `--sys-clock` | `2.5GHz` | benchmark runner | System/Ruby clock domain. |
| `fetchWidth` | `5` | benchmark runner `-P` | Frontend fetch width. |
| `decodeWidth` | `5` | benchmark runner `-P` | Decode width, set to track the 5-wide frontend. |
| `renameWidth` | `5` | benchmark runner `-P` | Rename width, set to track the 5-wide frontend. |
| `dispatchWidth` | `8` | benchmark runner `-P` | Dispatch width into IEW. |
| `issueWidth` | `8` | benchmark runner `-P` | Issue width. |
| `wbWidth` | `8` | benchmark runner `-P` | Writeback width. |
| `commitWidth` | `4` | benchmark runner `-P` | Commit/retire width. |
| `numROBEntries` | `128` | benchmark runner `-P` | ROB capacity. |
| instruction queue entries | `80` | benchmark runner `-P system.cpu[*].instQueues[0].numEntries=80` | Scheduler/IQ capacity in this gem5 tree. |
| `LQEntries` | `32` | benchmark runner `-P` | Regular load queue capacity. |
| `SQEntries` | `48` | benchmark runner `-P` | Regular store queue capacity. |
| `SPMLQEntries` | `32` | benchmark runner `-P` | SPM load queue capacity. |
| `SPMSQEntries` | `48` | benchmark runner `-P` | SPM store queue capacity. |
| `numPhysIntRegs` | `128` | benchmark runner `-P` | Physical integer register file size. |
| `numPhysFloatRegs` | `192` | benchmark runner `-P` | Physical floating-point register file size. |
| `numPhysVecRegs` | `192` | benchmark runner `-P` | Physical vector register file size. |
| `numPhysVecPredRegs` | `64` | benchmark runner `-P` | Physical predicate register file size. |

These knobs matter because SPM can shift the bottleneck from memory-system stalls to frontend/backend/core-resource limits. For best-to-best comparison, the baseline and SPM runs should use identical core parameters.

### 21.7 SVE and vector-length knobs

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| `sve_vl_se` | `16` for current old 8x3VL GEMM path | benchmark runner `-P system.cpu[*].isa[0].sve_vl_se=16` | gem5 SVE vector length setting. In gem5, this is in 128-bit chunks, so `16` means 2048-bit SVE. |
| `SPM_VL` | `16` | build script/env | Compile-time selection for the SPM kernel path. |
| Kernel shape | `8x3VL` | `cacheflex_micro` SPM kernel headers and GEMM code | Microkernel computes 8 rows by 3 vector-length columns per inner kernel instance. |

`sve_vl_se` and `SPM_VL` must match. If gem5 simulates a different SVE vector length than the compiled SPM kernel expects, the kernel can fail correctness checks or use the wrong SPM layout.

The current GEMM comparison intentionally keeps the old `8x3VL`, `VL16` path because that is the path matching the preserved `cacheflex_micro`-style SPM implementation and prior W1-W7 tile choices.

### 21.8 SPM protocol behavior knobs and policy choices

These are not all command-line knobs, but they are implementation policy choices that can be changed in the protocol.

| Policy / mechanism | Current behavior | Where implemented | Effect |
|---|---|---|---|
| SPM stable state | `X` | `MESI_Three_Level_SPM-L1cache.sm` | Marks private scratchpad slots outside the coherence domain. |
| Silent copy-in request | `GETS_SILENT` | L1/L2/directory SLICC files | Fetches a coherent data snapshot without adding the SPM requester as a sharer. |
| Source release messages | `PUTS`, `PUTM`, `PUTE` | L1/directory SLICC files | Releases or writes back the coherent source after SPM copy-in when needed. |
| SPM writeback request | `SPMWB_REQ` | L1/directory SLICC files | Writes SPM data back to a coherent destination. |
| SPM writeback ack | `SPMWB_ACK` | directory/L1 SLICC files | Completes an outstanding SPM writeback. |
| SPM response path | dedicated `spmBufferToL0` | `MESI_Three_Level.py`, SPM L0/L1 SLICC | Separates SPM responses from ordinary L1-to-L0 coherence responses. |
| Absent SPM read | returns zero data | `MESI_Three_Level_SPM-L1cache.sm`, `CacheMemory::readSPMData` | Provides deterministic behavior for absent/non-SPM slot reads in the model. |
| SPM reinstall without release | panic | `CacheMemory::migrateOrClearSPMSlot` | Catches software/runtime bugs where a live SPM slot is overwritten without explicit release. |
| Live coherent occupant in requested SPM slot | migrate to free non-SPM way | `CacheMemory::migrateOrClearSPMSlot` | Preserves coherent data if possible while claiming the requested SPM set/way. |
| Normal replacement victim selection | excludes SPM entries and reserved SPM ways | `CacheMemory::cacheProbe` | Prevents normal cache replacement from evicting software-managed SPM slots. |

These choices define the semantics of SPM in the multicore Ruby model. They are more important than ordinary benchmark knobs because changing them can change correctness, not just performance.

### 21.9 Hardware prefetching knobs

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| `--enable-prefetch` | enabled in the active runner | benchmark runner | Enables Ruby/configured prefetch behavior for the baseline and SPM runs. |

For fair comparison, prefetching should be either enabled for both baseline and SPM or disabled for both. The current runner keeps it enabled for both, matching the intent of comparing SPM against a reasonably strong cache baseline rather than an artificially weakened one.

### 21.10 GEMM benchmark/runtime knobs

| Knob | Current value | Where set | Purpose / effect |
|---|---:|---|---|
| `THREADS` | `8` | benchmark runner env/default | Number of worker threads and simulated cores used by GEMM. |
| `SPM_WAYS` | `4` | benchmark runner env/default | Number of SPM-reserved private-L2 ways for SPM runs. Passed to `--l1d_num_spm_ways`. |
| `WARMUP` | `0` | benchmark runner env/default | Number of unmeasured warmup repetitions. |
| `REPEAT` | `1` | benchmark runner env/default | Number of measured repetitions. |
| `VERIFY` | `16` | benchmark runner env/default | Verification sample/count control for GEMM correctness checking. |
| `MAXJOBS` | `1` | benchmark runner env/default | Host-side parallelism for launching gem5 jobs. Does not change simulated core count. |
| `M`, `K`, `N` | W1-W7 workload table | benchmark runner | GEMM shape. |
| `mc` | shape-specific | benchmark runner tile table | M-dimension macro tile size. |
| `kc` | shape-specific | benchmark runner tile table | K-dimension panel/tile size. |
| `grid_cols` | shape-specific | benchmark runner tile table | Column partitioning of C tiles across the thread grid. |

Current optimal-only tile choices in the runner:

| Shape | Baseline tile | SPM tile |
|---|---|---|
| W1 | `mc128 kc256 grid8` | `mc128 kc128 grid8` |
| W2 | `mc96 kc64 grid8` | `mc96 kc128 grid8` |
| W3 | `mc96 kc128 grid4` | `mc96 kc256 grid4` |
| W4 | `mc96 kc64 grid2` | `mc96 kc256 grid2` |
| W5 | `mc96 kc128 grid2` | `mc96 kc256 grid2` |
| W6 | `mc96 kc64 grid2` | `mc96 kc64 grid2` |
| W7 | `mc96 kc128 grid2` | `mc96 kc128 grid2` |

These are benchmark-specific tuning parameters, not Ruby implementation parameters. They control how much data reuse is exposed to SPM and how work is partitioned across cores.

### 21.11 Correctness and safety checks

| Check | Where implemented | Purpose |
|---|---|---|
| `num_spm_ways < assoc` | `CacheMemory::init` | Ensures at least one coherent way remains. |
| encoded SPM set matches cache set | `CacheMemory::allocateSPMSlot` | Ensures software's decoded SPM set agrees with Ruby's cache indexing. |
| SPM way within associativity | `CacheMemory::allocateSPMSlot` | Prevents illegal way placement. |
| no normal allocation of scratchpad entries | `CacheMemory::allocate` | Keeps SPM entries out of ordinary cache fill paths. |
| no normal deallocation of scratchpad entries | `CacheMemory::deallocate` | Forces SPM slots to be released through the SPM path. |
| no replacement of SPM lines | `CacheMemory::cacheProbe` | Ensures normal coherent replacement never selects an SPM slot. |
| no reinstall over live SPM slot | `CacheMemory::migrateOrClearSPMSlot` | Catches missing `SPM_release` bugs. |
| SPM requests bypass physical-memory backing-store access | `RubyPort::hitCallback` | Prevents encoded SPM addresses from reading/writing gem5 backing memory directly. |

These checks are intentionally strict. They make protocol/runtime bugs fail loudly instead of silently corrupting coherence or SPM state.

### 21.12 Parameters that must remain consistent

Several knobs are coupled and should not be changed independently:

```text
SPM_VL == sve_vl_se
```

The SPM kernel compile-time vector length must match gem5's simulated SVE vector length.

```text
l1d_size/l1d_assoc/cacheline_size == SPM address encoding geometry
```

The current encoding assumes 1024 sets, 8 ways, and 64B slots. That corresponds to 512KB, 8-way, 64B-line private L2/SPM.

```text
baseline and SPM core parameters must match
```

Fetch/decode/rename/dispatch/issue/writeback/commit widths, ROB/IQ/LSQ/RF sizes, clocks, and prefetch settings should be identical between baseline and SPM. Otherwise speedups mix SPM effects with core-model differences.

```text
l1d_num_spm_ways must be zero for baseline and nonzero for SPM
```

The baseline should not reserve SPM ways, because doing so would reduce coherent cache capacity without giving the baseline access to SPM operations.

```text
SPM_WAYS <= l1d_assoc - 1
```

At least one coherent way must remain. In practice, more than one coherent way is needed for meaningful baseline/SPM comparison.

```text
SPM slot lifecycle: install/fetch -> load/store/read/writeback -> release
```

Software must not reinstall over a live SPM slot without releasing it first.
