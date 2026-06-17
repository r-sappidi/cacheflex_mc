# CacheFlex SPM: Microarchitecture

This describes the **hardware** a CacheFlex SPM core would be — the physical
structures, datapaths, control state machines, and timing — as inferred from
the gem5 `MESI_Three_Level_SPM` behavioral model. The gem5 code is the
executable spec; this document is what you would actually build. Where gem5
"cheats" (zero-latency magic, oracle data movement), the real-hardware
equivalent is called out.

## 1. System context

The modeled machine is a tiled multicore:

- **Core:** ARM out-of-order, SVE (512-bit vectors).
- **L1 (core-private, split I/D):** 64 KB, 4-way — gem5 calls this `L0`.
- **Private last-level cache (PLLC), unified:** 512 KB, 8-way — gem5 calls this
  `L1`. **This array physically holds the scratchpad.**
- **Shared LLC + home/snoop filter:** tiled (e.g. 8 × 512 KB slices) — gem5
  `L2`; this is the coherence home.
- **Directory / memory controllers** behind the LLC.
- **NoC:** mesh or crossbar.

The scratchpad is not a separate SRAM macro. It is a **mode of the PLLC**: a
configurable number of *ways per set* are removed from the coherent cache and
operated as a directly-indexed scratchpad. This is the whole design thesis —
reuse the existing tag/data arrays rather than add a tightly-coupled memory
(TCM) macro, so the boundary between cache and scratchpad is a configuration
register, not a wire.

## 2. SPM storage: the way-partitioned PLLC

### 2.1 Physical organization

The PLLC data and tag SRAMs are unchanged. Three additions make a way-subset
behave as scratchpad:

1. **`SPM_WAYS` config register** (per cache): the low `W` ways
   `[0, W)` of *every* set are reserved. `W < associativity` always (at least
   one coherent way must remain). Sized by software as
   `W = ceil(panel_lines / num_sets)`.
2. **Per-line `SPM` status bit**, stored alongside the MESI state bits in the
   tag array. `SPM=1` marks a scratchpad line; it is mutually exclusive with a
   valid coherent MESI state.
3. **Replacement/allocation masking.** The victim-selection logic (PLRU/RRIP
   tree) and the fill-allocation logic are gated to consider only ways
   `[W, assoc)`. Reserved ways are structurally invisible to coherent
   allocation and eviction — this is what guarantees "a scratchpad line is
   never evicted," with no policy decisions, just a masked victim search.

### 2.2 Two access paths into the same arrays

| | Coherent access | SPM access |
|---|---|---|
| Index | physical line addr → set; **tag CAM across coherent ways** → way | slot address directly names **(set, way)** |
| Hit detect | tag compare + valid + permission | implicit — the way is given; check `SPM` bit |
| Energy/latency | full tag read + way mux | **no tag CAM**, direct way enable |

This is the key microarchitectural payoff and it is easy to miss in the gem5
model: an **SPM load/store is direct-mapped to a known way**, so it skips the
tag-comparison and way-selection tree entirely. It behaves like a TCM/scratchpad
access (single-ported SRAM read at a computed address), not a set-associative
cache lookup — lower latency and lower energy than even an L1 hit, while reusing
the L2 data array. The scratchpad index is the gem5 "encoded slot address"
`(way << 16) | (set << 6) | offset`; in hardware it is just the address bits
that drive the set decoder plus a way-enable, with no translation.

## 3. ISA and core-pipeline integration

### 3.1 Instructions

Seven scratchpad operations, encoded as repurposed load/store formats:

- **`SPMCP` (copy-into-SPM)** — the only one touching coherence. `SPMCP Xslot,
  [Xsrc]`: `Xsrc` is a normal (translated) address of a coherent line; `Xslot`
  is the scratchpad index. Semantics: pull the line out of coherence and place
  its data at `(set, way)`.
- **`SPMINSTALL`** — claim a slot without fetching (software will fill it).
- **`SPMLD` / `SPMST`** — scratchpad load/store; address operand *is* the slot
  index, untranslated (`PHYSICAL`). SVE variants stream a packed panel.
- **`SPMWB`** — write a dirty slot back toward memory.
- **`SPMRELEASE`** — return a slot's way to the coherent pool.

### 3.2 Two addresses, one uop

`SPMCP` is microarchitecturally a **two-source-register memory uop**: it reads
`Xsrc` (drives the AGU + TLB like a normal load) *and* `Xslot` (the scratchpad
index, used raw). In an OoO core it allocates an LSQ entry and an **SPM-fetch
tracker** (an MSHR-class structure, §5.2). The slot index never enters the TLB;
only the coherent source is translated. `SPMLD/SPMST` are single-address uops
whose address is the slot index and which are marked non-translating.

### 3.3 LSU / memory pipeline

- SPM ops occupy a normal load or store pipe and AGU port.
- `SPMLD/SPMST` resolve in the PLLC's SPM access path (§2.2): one array access,
  no tag CAM, no coherence — they are the fast path the workload spends its time
  in.
- `SPMCP/SPMWB/SPMRELEASE/SPMINSTALL` are "slow" control ops that hand off to the
  PLLC SPM controller and retire when it acks.
- **Speculation safety:** because `SPMLD` is non-coherent and direct-indexed, a
  mis-speculated or pre-fence `SPMLD` to an unclaimed slot must not fault. The
  modeled behavior is *return architected zero* (load) / *silently drop* (store)
  on a slot whose `SPM` bit is clear. In silicon this is the LSU treating
  "`SPM` bit == 0" as a benign miss that completes with zeros, so O3 ghost
  accesses never raise. Correct results depend on the software fence contract
  (§7), not on hardware coherence.

## 4. Coherence-fabric extensions

New request opcodes and one new response, carried on the existing coherent
interconnect (they need request/response virtual networks but **no new data
network**):

- **`GETS_SILENT`** — "non-tracking read": fetch the line's data but the home
  must **not** record the requester as a sharer. The defining property: the
  returned copy will never be snooped, because it is leaving the coherence
  domain.
- **`PUTS` / `PUTE` / `PUTM`** — *source release*: tell the home this core is
  relinquishing its Shared / Exclusive / Modified copy of the source line so it
  can be moved into SPM. `PUTM` carries dirty data.
- **`PUT_ACK`** — home acknowledges the release is globally visible.
- **`SPMWB_REQ` / `SPMWB_ACK`** — scratchpad-to-memory writeback path.

Each message also carries the SPM slot index so trackers can route the eventual
fill, but the fabric/home treat the slot index as opaque metadata, never as a
coherent address.

## 5. PLLC SPM controller (the "SPM engine")

This is the private-cache coherence controller, extended with a scratchpad
sub-FSM. It is the most interesting new block.

### 5.1 Line state

Each PLLC line is `{MESI state, SPM bit}`. The scratchpad-resident state is
`X` (`SPM=1`, no coherent permission). Around it sit transient states tracked
**per in-flight op in the SPM-fetch tracker**, not per line:

- `IX_D` — fetching a remote/absent source via `GETS_SILENT`.
- `SX_L0 / EX_L0 / MX_L0` — back-invalidating the inclusive L1 (`L0`) copy
  before release.
- `SX_A / EX_A / MX_A` — release (`PUT*`) sent, awaiting `PUT_ACK`.
- `XWB` — writeback outstanding.

### 5.2 SPM-fetch tracker (MSHR-class)

A small table (a handful of entries) mirroring the gem5 TBEs. Each entry holds:
source line address, slot (set, way), original coherence state, buffered data,
and the transient FSM state. This is the structure that lets `SPMCP` be
non-blocking and that participates in the snoop pipeline (§5.5).

### 5.3 `SPMCP` datapath, by source state

The controller branches on the source line's local state:

- **Not present locally (`I`)** → issue `GETS_SILENT` to the home (`IX_D`). On
  data return, write it into the slot data array, set `SPM=1`, ack the core.
- **Present in the inclusive L1/`L0` too (`S/E/M`)** → first **back-invalidate
  the L1 copy** (inclusion recall): `SX_L0/EX_L0/MX_L0`. For `M`, the recall
  also pulls the freshest dirty bytes up. Then issue `PUTS/PUTE/PUTM` to the
  home (`*X_A`).
- **Present only in the PLLC (`S/E/M` clean of L1)** → skip the recall, issue
  the `PUT*` directly.
- On `PUT_ACK`, **promote the existing cache line in place to a scratchpad
  line**: flip its `SPM` bit, drop its coherent permission, deallocate the
  coherent tag. No data movement is needed if the line already sat in a reserved
  way; otherwise see §5.4.

After this, the source address has **no coherent copy anywhere in this core**,
and the data is reachable only by slot index. That is the traffic win: the
reused datum stops participating in coherence and stops being refetched.

### 5.4 Slot install / occupant migration

The slot index names a fixed `(set, way)`. If that way currently holds a live
coherent line, the controller **relocates the occupant to another (non-reserved)
way in the same set** by a tag/state-array update — the data either stays put
with a way-pointer remap or is copied within the set. Crucially this emits **no
coherence traffic**: the line keeps its MESI state and address; only its way
position changes. Hardware cost: a free-way search within the set and a tag
write. Software guarantees a free coherent way exists (the `SPM_WAYS < assoc`
invariant plus the panel-sizing contract). Re-installing over an occupied
scratchpad slot is illegal (must `SPMRELEASE` first) — a real machine would
fault or assert.

### 5.5 Snoop participation (the silent-snapshot mechanism)

This is the subtle part that makes read-shared data work. While a core holds a
line (as `M/E`, or mid-`SPMCP` in `MX_A/EX_A` still holding it), the home may
forward another core's request to it:

- **Normal forwarded `GETS`** to a core in `MX_A/EX_A`: it can still answer —
  supply data to the requester and the LLC, and **downgrade to `SX_A`** (it
  keeps a shared copy until its own release completes). Forwarded `GETX/INV`
  must wait until this core's own release drains.
- **Forwarded `GETS_SILENT`** (another core's `SPMCP` snapshotting this owner's
  line): the owner returns a **data copy plus an `Unblock`, with its own
  coherence state unchanged**. It does not invalidate, does not become a sharer
  of anything, does not give up ownership. This is a *snoop that supplies data
  without changing owner state* — a "stash/forward-without-invalidate" response.

In silicon this means the SPM-fetch tracker and the line-state array both feed
the snoop-response logic, and there is a new snoop-response type
("data, no-state-change, no-sharer-add"). This is what lets every core snapshot
the same read-shared B-panel into its own scratchpad while the producing core
keeps its single coherent copy intact — no replication-and-invalidate storm.

## 6. Home / LLC directory extensions

The shared-LLC home (snoop filter + directory) gains a few behaviors and
transient states:

- **`GETS_SILENT` servicing:**
  - LLC miss → fetch from memory (`SPM_IS` transient), forward the snapshot,
    and **do not install a tracked copy** (drop back to not-present) so no
    untracked line can later cause a spurious back-invalidate.
  - LLC hit, no on-chip owner → supply data, **do not add a sharer**.
  - On-chip owner exists → forward `GETS_SILENT` to the owner (`MT_SPMS`
    transient), wait for the owner's `Unblock`, then return to the owner-intact
    state. The SPM requester is **never added to the sharer vector**.
- **`PUTS/PUTE/PUTM` servicing:** clear the requester from the sharer vector /
  clear ownership; for `PUTM`, absorb dirty data; send `PUT_ACK`. The line stays
  cached at the LLC (now with no on-chip coherent owner).
- **`SPMWB_REQ`:** write the slot's dirty data to the LLC/memory (`SPM_WB`
  transient), then `SPMWB_ACK`.
- **Sharer-vector suppression** is the one genuinely new directory invariant: an
  SPM requester must never appear as a sharer or owner. Everything else reuses
  existing forward/ack machinery.

The memory-side directory only sees true misses/writebacks (`SPM_IS`/`SPM_WB`/
source-release dirty writebacks) and likewise never tracks SPM slots.

## 7. Memory ordering and consistency

The scratchpad is outside the coherence domain, so ordering is software-managed:

- A **`DSB`-class fence** is required (a) after `SPMCP`/`SPMINSTALL` before the
  first `SPMLD` to that slot, and (b) before reusing/`SPMRELEASE`-ing a slot, so
  the non-coherent slot view is synchronized with the coherent fetch.
- The core keeps **per-slot ordering** for SPM ops and **source-address
  ordering** for `SPMCP` release, so a younger `SPMLD` cannot observe a slot
  before the matching `SPMCP/SPMINSTALL` retires.
- Because SPM data is non-coherent, correctness on read-shared data relies on
  the program model (snapshot-then-read), not on hardware invalidation. Stores
  by another core to the original source line after a snapshot are **not** seen
  through the scratchpad — by design, this is a producer/consumer or
  read-only-during-region pattern.

## 8. Timing model (cycle intuition)

| Operation | Dominant cost |
|---|---|
| `SPMLD` / `SPMST` hit | one PLLC data-array access, **no tag CAM** → ~private-cache data latency or better (TCM-like) |
| `SPMCP` from local `S/E/M` | L1 back-invalidate + one home round-trip (`PUT*`→`PUT_ACK`); **no DRAM** |
| `SPMCP` from local-miss `I` | home + **memory** latency (silent DRAM fetch) |
| `SPMCP` silent-snapshot of a remote owner | NoC: requester→home→owner→requester (on-chip, no DRAM) |
| `SPMINSTALL` | slot claim + possible intra-set occupant migration (tag write) |
| `SPMWB` | home + memory write latency |
| `SPMRELEASE` | tag update; way rejoins coherent pool |

The intended steady state is: pay the `SPMCP` cost once per reused panel, then
amortize it over many cheap `SPMLD`s that neither miss, refill, nor coherence-
probe — which is exactly the measured effect (private-cache miss rate collapsing
and coherence-control NoC traffic dropping ~98% on the GEMM panels).

## 9. Area / power / complexity budget

- **Storage:** ~free for data — it reuses the PLLC SRAM. Adds **1 status bit per
  line** (`SPM`) and one `SPM_WAYS` config register per cache.
- **SPM-fetch tracker:** a few MSHR-width entries (source addr, slot, state,
  data buffer).
- **Control:** extra transient states in the private-cache FSM and a handful in
  the LLC/directory FSM; one new snoop-response type; sharer-suppress gating.
- **Fabric:** a few new request/response opcodes (reusing existing virtual
  networks); no new physical links.
- **Cost to coherent performance:** the reserved ways reduce the effective
  associativity and capacity of the private cache for normal data — the real
  tradeoff the architect tunes via `SPM_WAYS`.

## 10. What gem5 abstracts vs. real silicon

- **Occupant migration "with no coherence message"** — in gem5 it is an instant
  array edit. In hardware it is a real intra-set way remap (free-way search +
  tag write), correct because the line never leaves the cache; it is cheap but
  not literally free.
- **Zero-on-absent-slot** — gem5 returns architected zero to tolerate O3 ghost
  loads. Silicon needs the LSU to treat `SPM`-bit-clear as a benign zero-return
  miss (or to guarantee, via fences, such accesses never commit).
- **Direct-indexed SPM access latency** — gem5 charges generic cache latency;
  real hardware can make `SPMLD/SPMST` *faster* than a cache hit by skipping the
  tag-CAM/way-mux, which gem5 does not currently model.
- **Magic data buffering in trackers** — gem5 carries a full `DataBlk` in the
  TBE; hardware uses an MSHR data buffer of the same line width.
- **Slot-index encoding** `(way<<16)|(set<<6)` is a gem5 software ABI; real
  hardware would expose a flatter scratchpad address space and bit-slice it into
  the set decoder + way enable.

## 11. One-paragraph summary

CacheFlex SPM is a **way-partitioned private last-level cache** that lets
software pull selected lines out of the coherence domain into directly-indexed
scratchpad ways. The new hardware is modest: a way-reservation register and per-
line SPM bit (turning the existing data array into part-cache/part-scratchpad), a
small SPM-fetch tracker in the private-cache controller, a "non-tracking read"
(`GETS_SILENT`) plus source-release (`PUT*`) opcodes on the coherence fabric, and
a sharer-suppression rule at the home. The payoff is that read-shared, reused
data (packed GEMM panels) is snapshotted once per core — via a snoop that
supplies data without disturbing the owner — and then accessed as fast, non-
coherent, never-evicted scratchpad, eliminating the refill and coherence traffic
that a normal cache would generate for the same access pattern.
