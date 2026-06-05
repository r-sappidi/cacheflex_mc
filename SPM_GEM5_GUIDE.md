# CacheFlex SPM ISA & gem5 Stats Collection Guide

## Table of Contents
1. [SPM Architecture Overview](#1-spm-architecture-overview)
2. [SPM ISA — Instruction Encoding](#2-spm-isa--instruction-encoding)
3. [Scalar SPM Instructions](#3-scalar-spm-instructions)
4. [SVE Vector SPM Instructions](#4-sve-vector-spm-instructions)
5. [Assembly & Compiler Flow](#5-assembly--compiler-flow)
6. [Usage Pattern: Pack → Barrier → Load](#6-usage-pattern-pack--barrier--load)
7. [gem5 ROI Stats Collection](#7-gem5-roi-stats-collection)
8. [Key Stats in stats.txt](#8-key-stats-in-statstxt)
9. [gem5 Run Configuration](#9-gem5-run-configuration)
10. [Known ISA Gotchas](#10-known-isa-gotchas)

---

## 1. SPM Architecture Overview

CacheFlex exposes a **Scratchpad Memory (SPM)** backed by repurposed L2 cache ways.

```
L2 Cache: 512 KB, 8-way set-associative, 1024 sets, 64B/line
SPM allocation: up to 3 ways => 192 KB addressable scratchpad
```

**SPM address format** (64-bit virtual address passed to SPMCP/spm.ld1qd):

```
bits [63:17]  reserved / zero
bits [16:16]  way_id  (one bit in simple 3-way; extended for VL=8/16)
bits [15:6]   set_index  (10 bits => 1024 sets)
bits [5:0]    byte offset within set (64B line)
```

Equivalently:
```c
spm_addr = (way_id << 16) | (set_index << 6) | byte_offset;
```

**Per-VL SPM layout** (NT = FP16 elements per B-row loaded in one spm.ld1qd):

| VL  | SVE width | svcnth() | NT  | Ways used | SPMCP size |
|-----|-----------|----------|-----|-----------|------------|
| 2   | 256-bit   | 16       | 48  | 3 (Way0/1/2) | 32B (SPMCP_32_IMM) |
| 4   | 512-bit   | 32       | 96  | 3 (Way0/1/2) | 64B (SPMCP_64_IMM) |
| 8   | 1024-bit  | 64       | 192 | 4 (Way0-3, KC-offset reuse) | 128B (SPMCP_64_IMM x2) |
| 16  | 2048-bit  | 128      | 384 | 4 (Way0-3, 3x KC-level stacking) | 256B (SPMCP_64_IMM x4) |

---

## 2. SPM ISA -- Instruction Encoding

All SPM scalar instructions are 32-bit, with the top byte `0xFF` and bit[24]=1:

```
 31      24  23:22  21:20  19:18  17:16  15:10   9:5   4:0
 11111111    op     size2  addrMd  ext2   imm6    rn    rt
```

| Field    | Bits    | Meaning |
|----------|---------|---------|
| [31:24]  | 8b      | Always `0xFF` |
| [24]     | 1b      | Must be `1` to enter `decodeSPM()` |
| op       | [23:22] | `00`=LDR, `01`=STR, `10`=CP (SPMCP), `11`=WB |
| size2    | [21:20] | `0`=1B, `1`=2B, `2`=4B, `3`=use ext2 for CP/WB |
| addrMd   | [19:18] | `0`=IMM, `1`=POST, `2`=PRE |
| ext2     | [17:16] | (only when size2=3 & op>=2): `0`=8B, `1`=16B, `2`=32B, `3`=64B |
| imm6     | [15:10] | Scaled immediate offset (0-63 units) |
| rn       | [9:5]   | Base address register |
| rt       | [4:0]   | Data register (or SPM slot register for CP/WB) |

**Immediate byte offset** = `imm6 x elemBytes`

where `elemBytes` is determined by size2/ext2:
- size2=0 -> 1B; size2=1 -> 2B; size2=2 -> 4B; size2=3 -> ext2 field applies
- For op=CP/WB with size2=3: ext2=0->8B, ext2=1->16B, ext2=2->32B, ext2=3->64B

---

## 3. Scalar SPM Instructions

### SPMCP (Copy from DRAM to SPM) -- the primary packing instruction

```asm
SPMCP_64_IMM  rt, [rn, #byte_offset]
SPMCP_32_IMM  rt, [rn, #byte_offset]
```

- `rn` = source address in DRAM (normal virtual address)
- `rt` = SPM destination address: `(way_id << 16) | (set_index << 6)`
- `#byte_offset` = immediate byte offset on the SPM address
  - `#0`   -> imm6=0 -> Way0 of current set
  - `#64`  -> imm6=1 -> Way1 of current set  (SPMCP_64)
  - `#128` -> imm6=2 -> Way2 of current set  (SPMCP_64)
  - `#32`  -> imm6=1 -> Way1 of current set  (SPMCP_32)

> **Critical**: Use `IMM` mode, **not** `PRE` mode.
> `PRE` has writeback: `rn <- rn + imm` after each instruction, causing cumulative
> address drift across sequential SPMCP calls targeting different ways.

### SPMLDR / SPMSTR (scalar load/store to SPM)

```asm
SPMLDR_8_IMM   rt, [rn, #offset]   # load 8 bytes from SPM
SPMSTR_4_POST  rt, [rn, #offset]   # store 4 bytes to SPM, post-increment
```

> **ISA encoding conflict**: SPMLDR (op=00) and SPMSTR (op=01) share the
> encoding space with gem5 m5 pseudo-ops (m5_work_begin etc. have bits[23:22]=00/01
> and bits[15:0]=0x0110). The ISA decoder routes op>=2 (SPMCP, SPMWB) to
> `decodeSPM()` and falls through to `Gem5Op64` for op<2. Do not use
> SPMLDR/SPMSTR in kernels that also use gem5 ROI macros.

### SPMWB (Writeback SPM to DRAM)

```asm
SPMWB_64_IMM   rt, [rn, #offset]
```

---

## 4. SVE Vector SPM Instructions

### spm.ld1qd -- the primary SPM load

Loads one SPM way (64B or 128B depending on VL) into an SVE Z register:

```asm
spm.ld1qd  z2.d, p1/z, [x16]    # load 64B (VL=4) or 128B (VL=8) from SPM
```

- `x16` = SPM address: `(way_id << 16) | (set_index << 6)`
- At VL=4 (512-bit SVE): loads 64B = 32 FP16 elements per call
- At VL=8 (1024-bit SVE): loads 128B = 2 consecutive ways per call

The SPM address must have been populated via SPMCP before this executes.
A `dsb sy` barrier between the SPMCP loop and spm.ld1qd is mandatory (Section 6).

### Other SVE SPM variants

```asm
spm.ld1b   z0.b, p0/z, [sp, #0, mul vl]    # load bytes
spm.ld1h   z1.h, p1/z, [x1, #2, mul vl]    # load halfwords
spm.ld1w   z2.s, p2/z, [x2, #4, mul vl]    # load words
spm.ld1d   z3.d, p3/z, [x3, #8, mul vl]    # load doublewords
spm.st1b / spm.st1h / spm.st1w / spm.st1d  # corresponding stores
spm.ld1rqb / spm.ld1rqh / ...              # replicate quadword variants
```

---

## 5. Assembly & Compiler Flow

SPM instructions cannot be assembled by standard GCC/LLVM. The CacheFlex
`spm_compiler.py` post-processes the assembly to encode SPM pseudo-mnemonics
into `.inst 0xXXXXXXXX` directives.

### 3-step build flow

```bash
# Step 1: Compile C++ to assembly
aarch64-none-linux-gnu-g++ -O3 -S -o kernel.s kernel.cpp \
    -march=armv8.2-a+sve+fp16 -DVL_4 -DGEM5

# Step 2: Encode SPM instructions
python3 ${BENCH_ROOT}/Cacheflex/cacheflex_sw/spm_compiler.py \
    kernel.s kernel_enc.s

# Step 3: Assemble + link
aarch64-none-linux-gnu-g++ -o bin_gem5/kernel kernel_enc.s \
    -march=armv8.2-a+sve+fp16 \
    ${BENCH_ROOT}/gem5/util/m5/build/arm64/out/m5op.o \
    -DGEM5 -I${BENCH_ROOT}/gem5/include
```

### spm_compiler.py immediate encoding rule

The compiler expects **byte values** for the immediate offset in assembly source:

```asm
# SPMCP_64: elem_bytes=64, so imm6 = byte_offset / 64
SPMCP_64_IMM  x14, [x12, #0]     # imm6=0 -> Way0, offset 0B
SPMCP_64_IMM  x14, [x12, #64]    # imm6=1 -> Way1, offset 64B
SPMCP_64_IMM  x14, [x12, #128]   # imm6=2 -> Way2, offset 128B

# SPMCP_32: elem_bytes=32, so imm6 = byte_offset / 32
SPMCP_32_IMM  x16, [x12, #0]     # imm6=0 -> Way0, offset 0B
SPMCP_32_IMM  x14, [x12, #32]    # imm6=1 -> Way1, offset 32B
SPMCP_32_IMM  x14, [x12, #64]    # imm6=2 -> Way2, offset 64B
```

> **Do not** use unit notation `#1, #2` (as seen in some reference gemm_vl_*.cpp).
> spm_compiler.py treats the immediate as bytes, so `#1` encodes as `imm6=0`,
> making all three SPMCP calls target Way 0.

---

## 6. Usage Pattern: Pack -> Barrier -> Load

The canonical SPM usage pattern in the GEMM micro-kernel:

```cpp
// ---- pack_B_tile_to_spm() ----
// SPM addresses for 3 ways at set k
uint64_t spm_base = (set_k << 6);              // set index
uint64_t way0 = spm_base | (0ULL << 16);
uint64_t way1 = spm_base | (1ULL << 16);
uint64_t way2 = spm_base | (2ULL << 16);

asm volatile(
    // Barrier 1: ensure previous spm.ld1qd from last iteration completes
    // before SPMCP overwrites the same SPM sets with new B data.
    "dsb sy\n"

    // Copy 3 x 64B slices of B_row[k] into 3 SPM ways
    "SPMCP_64_IMM x14, [x12, #0]\n"    // Way0 <- B[k, n+ 0..31]
    "SPMCP_64_IMM x14, [x12, #64]\n"   // Way1 <- B[k, n+32..63]
    "SPMCP_64_IMM x14, [x12, #128]\n"  // Way2 <- B[k, n+64..95]

    // Barrier 2: ensure CPTOSPMResp arrives (DRAM->SPM DMA completes)
    // before spm.ld1qd reads. Without this, the O3 CPU can issue
    // spm.ld1qd speculatively before SPMCP finishes (different address
    // spaces => no automatic RAW hazard detection in the O3 LSQ).
    "dsb sy\n"
    : : "r"(way0), "r"(B_ptr) : "x12", "x14", "memory"
);

// ---- spm_kernel() ----
asm volatile(
    "dsb sy\n"                             // barrier at kernel entry

    "spm.ld1qd z2.d, p1/z, [x16]\n"      // z2 <- Way0 (32 FP16 @ VL=4)
    "spm.ld1qd z3.d, p1/z, [x15]\n"      // z3 <- Way1
    "spm.ld1qd z4.d, p1/z, [x14]\n"      // z4 <- Way2

    // 8x3 outer-product: z8-z31 are 24 FP16 accumulators
    "fmla z8.h,  p0/m, z0.h, z2.h[0]\n"
    // ... (24 accumulators, 3 col groups)
    : : : "memory"
);
```

**Why two DSBs are necessary:**

| Barrier | Location | Prevents |
|---------|----------|----------|
| DSB #1 (before SPMCP) | End of pack loop | Prior spm.ld1qd reading stale SPM after new SPMCP overwrites |
| DSB #2 (after SPMCP) | End of pack / start of kernel | spm.ld1qd executing before CPTOSPMResp (O3 speculation across address spaces) |

---

## 7. gem5 ROI Stats Collection

### ROI macros (common_spm.hpp)

```cpp
#ifdef GEM5
#  include <gem5/m5ops.h>
#  define ROI_BEGIN()  m5_work_begin(0, 0)
#  define ROI_END()    m5_work_end(0, 0)
#else
#  define ROI_BEGIN()  do {} while (0)
#  define ROI_END()    do {} while (0)
#endif
```

### Typical benchmark driver pattern

```cpp
// 1. Warm-up pass (outside ROI): populate caches, JIT settle
run_kernel(args...);

// 2. ROI: the measured region
ROI_BEGIN();                      // m5_work_begin(0,0) -- starts ROI tracking
for (int i = 0; i < n_iter; i++)
    run_kernel(args...);
ROI_END();                        // m5_work_end(0,0)   -- ends ROI tracking
```

gem5 dumps stats automatically at `m5_work_end`. The `stats.txt` section
delimited by "Begin Simulation Statistics" corresponds to the ROI interval.

### Fine-grained per-phase stats

To profile individual phases (e.g. pack vs. kernel separately):

```cpp
m5_reset_stats(0, 0);    // clear counters before phase 1
pack_B_tile_to_spm(...);
m5_dump_stats(0, 0);     // flush phase-1 stats to stats.txt

m5_reset_stats(0, 0);    // clear counters before phase 2
spm_kernel(...);
m5_dump_stats(0, 0);     // flush phase-2 stats to stats.txt
```

Each `m5_dump_stats` call appends a new stats block to `<outdir>/stats.txt`.
Each `m5_reset_stats` call zeroes all counters so the next dump shows only
activity since the last reset.

### m5ops API reference

```c
// gem5/include/gem5/m5ops.h
void m5_work_begin(uint64_t workid, uint64_t threadid); // start ROI
void m5_work_end(uint64_t workid, uint64_t threadid);   // end ROI, auto-dump

void m5_reset_stats(uint64_t ns_delay, uint64_t ns_period); // zero counters
void m5_dump_stats(uint64_t ns_delay, uint64_t ns_period);  // write stats.txt
void m5_dump_reset_stats(uint64_t ns_delay, uint64_t ns_period); // dump + reset
// Pass (0, 0) for immediate, one-shot execution
```

### Build flags required

```bash
aarch64-none-linux-gnu-g++ ... \
    -DGEM5 \
    -I${BENCH_ROOT}/gem5/include \
    ${BENCH_ROOT}/gem5/util/m5/build/arm64/out/m5op.o
```

> Note: QEMU cannot execute SPM instructions (CacheFlex opcodes are custom).
> Binaries built with `-DGEM5` must be run under `gem5.opt`. Binaries without
> `-DGEM5` can run under QEMU for functional verification (ROI macros become no-ops).

---

## 8. Key Stats in stats.txt

After a successful run, `<outdir>/stats.txt` contains one or more sections,
each opened by `---------- Begin Simulation Statistics ----------`.

### Simulation time

```
simSeconds                    0.149337   # seconds simulated
simTicks                  149336552961   # ticks (1e12 ticks/s base frequency)
system.cpu.numCycles          223892884  # CPU cycles in ROI
```

Conversion at 1.5 GHz (667 ticks/cycle):
```
simSeconds = simTicks / 1e12
CPU cycles = simTicks / 667
```

### CPU throughput

```
system.cpu.ipc      2.332947   # instructions per cycle (higher is better)
system.cpu.cpi      0.428642   # cycles per instruction  (lower is better)
```

### Cache miss rates

```
system.cpu.dcache.overall_miss_rate::total   # L1 D-cache miss rate
system.l2.overall_miss_rate::total           # L2 unified miss rate
system.cpu.icache.overall_miss_rate::total   # L1 I-cache miss rate
```

### DRAM traffic

```
system.mem_ctrls.bytesReadSys                # total bytes read from DRAM
system.mem_ctrls.bytesWrittenSys             # total bytes written to DRAM
system.mem_ctrls.requestorReadBytes::cpu.data  # data reads (excl. prefetch)
system.mem_ctrls.requestorReadBytes::l2.prefetcher  # prefetch traffic
```

### Branch / squash stats (useful for ghost iteration analysis)

```
system.cpu.branchPred.condIncorrect    # branch mispredictions
system.cpu.squashedInstsIssued         # instructions issued then squashed
system.cpu.squashedInstsExamined       # instructions iterated during squash
```

### Quick summary grep

```bash
grep -E "simSeconds|cpu\.ipc|cpu\.cpi|overall_miss_rate|bytesReadSys|bytesWrittenSys" \
    <outdir>/stats.txt
```

---

## 9. gem5 Run Configuration

### Invocation template (from llama_bench_spm/run_spm.sh)

```bash
${GEM5_ROOT}/build/ARM/gem5.opt \
    --outdir=<output_dir> \
    ${GEM5_ROOT}/configs/deprecated/example/se.py \
    --cpu-type=DerivO3CPU \
    --sys-clock=1.5GHz  --cpu-clock=1.5GHz \
    --caches --l2cache \
    --l1d_size=64kB  --l1i_size=64kB  --l2_size=512kB \
    --cacheline_size=64 \
    --l1i_assoc=4  --l1d_assoc=4  --l2_assoc=8 \
    --mem-size=8GB \
    --l1i-hwp-type=StridePrefetcher \
    --l1d-hwp-type=StridePrefetcher \
    --l2-hwp-type=AMPMPrefetcher \
    -P 'system.cpu[0].isa[0].sve_vl_se=<VL>' \
    -P 'system.cpu[0].numROBEntries=128' \
    -P 'system.cpu[0].numIQEntries=80' \
    -P 'system.cpu[0].LQEntries=32' \
    -P 'system.cpu[0].SQEntries=48' \
    -P 'system.cpu[0].fetchWidth=4' \
    -P 'system.cpu[0].decodeWidth=4' \
    -P 'system.cpu[0].issueWidth=8' \
    -P 'system.cpu[0].dispatchWidth=8' \
    -P 'system.cpu[0].commitWidth=4' \
    -P 'system.cpu[0].numPhysIntRegs=128' \
    -P 'system.cpu[0].numPhysFloatRegs=192' \
    -P 'system.cpu[0].numPhysVecRegs=192' \
    -P 'system.cpu[0].cacheLoadPorts=2' \
    -P 'system.cpu[0].cacheStorePorts=1' \
    -c <binary> -o '<space-separated args>'
```

### SVE VL mapping

| `sve_vl_se` | SVE width | svcnth() | NT  | Binary suffix |
|-------------|-----------|----------|-----|---------------|
| 2           | 256-bit   | 16       | 48  | `_vl2`        |
| 4           | 512-bit   | 32       | 96  | `_vl4`        |
| 8           | 1024-bit  | 64       | 192 | `_vl8`        |
| 16          | 2048-bit  | 128      | 384 | `_vl16`       |

### Output directory structure

```
<outdir>/
  stats.txt      # simulation statistics
  config.ini     # full gem5 config snapshot
  config.json    # config in JSON format
  fs/            # SE-mode filesystem (usually empty)
```

---

## 10. Known ISA Gotchas

### 1. SPMCP_PRE writeback causes cumulative address drift

`SPMCP_64_PRE` writes back `rn <- rn + imm` after every execution. Three
sequential SPMCP_PRE calls for Way0/1/2 compound: after Way0, `rn` shifts by 64;
after Way1, by another 64; Way2 now has a wrong base address.
**Fix:** Always use `SPMCP_64_IMM` (no writeback).

### 2. m5 pseudo-op ISA encoding conflict

`m5_work_begin` encodes to `0xFF5A0110`. CacheFlex routes all `0xFF` instructions
with bit[24]=1 to `decodeSPM()`. Since m5 pseudo-ops have bits[23:22]=00 or 01
(SPMLDR/SPMSTR range), they were decoded as SPMSTR_2_PRE, triggering
`base.cc:1723 ScratchpadBit` assertion before any SPMCP was issued.

**Fix (in `aarch64.isa`):** Route only op>=2 (bits[23:22]>=2, i.e. SPMCP and
SPMWB) into `decodeSPM()`; op<2 falls through to `Gem5Op64`. This means
SPMLDR and SPMSTR are effectively unavailable in kernels using ROI macros --
use SPMCP for all SPM packing, and spm.ld1qd for all SPM reads.

### 3. O3 ghost SPMCP from pack_B underflow

`subs kc,kc,#1; bne 1b` -- when `kc` reaches 0, `subs` sets kc to UINT64_MAX
(not zero), so `bne` is predicted taken. The O3 CPU speculatively issues one
extra pack iteration targeting SPM set 1024+ (outside valid range), causing a
`CPTOSPMReq` to arrive at L2 with `blk==null`.

**Fix (in `cache.cc::serviceMSHRTargets`):** When `CPTOSPMReq` arrives and
`blk==null || !blk->isValid()`, `memset(dst, 0, spm_size)` and skip the SPM
write. The O3 CPU squashes the ghost on branch resolution; the zero-fill is
harmless.

### 4. O3 ghost spm.ld1qd from spm_kernel 2-unroll

`cmp x20,#2; bge 3b` in the 2-unrolled inner loop -- after the last k-inner
iteration (x20=1), O3 predicts `bge` taken and speculatively executes the
second unroll body, issuing `spm.ld1qd` from SPM set 384 (never SPMCP'd),
triggering `base.cc:1723 ScratchpadBit assertion`.

**Fix (in `base.cc`):** For `SPMLoadReq` to a set without the ScratchpadBit
set, return zeros instead of asserting. The ghost load is squashed by O3
on branch resolution.

### 5. O3 SPMCP -> spm.ld1qd race (no cross-address-space dependency tracking)

The O3 LSQ tracks RAW hazards within the same address space. SPMCP operates
on the DRAM coherence address (source), while spm.ld1qd operates on the SPM
slot address. These are different address spaces, so O3 sees no dependency
and can execute spm.ld1qd before the CPTOSPMResp (DRAM->SPM DMA) completes.

**Fix:** Insert `dsb sy` after the SPMCP loop and at the start of spm_kernel.
This forces in-order completion at the ISA level and prevents the race.

### 6. spm_compiler.py unit vs byte notation

Reference files `gemm_vl_*.cpp` use unit notation `#0, #1, #2` intending
offsets of `0x64B, 1x64B, 2x64B`. The spm_compiler.py interprets these as
raw bytes: `imm6 = 1 // 64 = 0`, encoding all three SPMCP calls as imm6=0,
which copies to Way 0 three times (Ways 1 and 2 receive no data).

**Fix:** Always write byte values in assembly: `#0, #64, #128` (SPMCP_64) or
`#0, #32, #64` (SPMCP_32). This is what the spm_compiler.py correctly encodes.

### 7. seqNum vs address-range dependency in shadow table

The gem5 O3 shadow table tracks pending SPMCP -> spm.ld1qd ordering. In NSP
batching, each K-outer iteration re-uses the same SPM sets (same addresses)
with new data. A range-only check incorrectly blocks a spm.ld1qd from reading
iteration k's data by a future SPMCP for iteration k+1 that targets the same
addresses -- the load then waits and reads wrong data.

**Fix:** The shadow table uses a seqNum filter: only SPMCP entries with
seqNum < load_seqNum can block the load. Future-iteration SPMCPs
(dispatched speculatively past the loop branch, with higher seqNum) are
ignored. The DSB barriers guarantee correctness; the seqNum filter prevents
the false stall.
