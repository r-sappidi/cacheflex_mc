# VER-06: gem5 runtime check of the VER-05 directory DMA fix

Stage: complete_requires_review; 31/31 accepted.

- old binary (retained, unpatched): `/data2/rsappidi/cacheflex_mc/results/retained_protocol_restart_20261002/build/gem5.opt` sha256 98381f9847429370
- new binary (retained + patch): `/data2/rsappidi/cacheflex_mc/results/gem5_dma_fix_20261013/build/gem5.opt` sha256 3206d195d4678a89
- DMA target pages (calibrated): 0x104000, 0x210000 (16 lines each)

## Guest cases

| Case | Binary | DMA | Expected | Outcome | Guest | DMA seen (copy,load) per core | DMA pkts sent/completed | M_DRD/M_DWR.SPM_Writeback | *_SWB.Memory_Ack | M_DRD/M_DWR.CleanReplacement | Panic |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ctrl_old | old | no  | pass | pass | PASS | [[0, 0], [0, 0]] | - | 0/0 | 0/0 | 0/0 | - |
| ctrl_new | new | no  | pass | pass | PASS | [[0, 0], [0, 0]] | - | 0/0 | 0/0 | 0/0 | - |
| dma_old | old | yes  | expected_panic | expected_panic | - | [] | - | 0/0 | 0/0 | 0/0 | system.ruby.dir_cntrl0 time: 62973 addr: 0x210100 event: SPM_Writeback state: M_DWR |
| dma_old_rd | old | yes --dma-read-percent=100 | expected_panic | expected_panic | - | [] | - | 0/0 | 0/0 | 0/0 | system.ruby.dir_cntrl1 time: 68009 addr: 0x2102c0 event: SPM_Writeback state: M_DRD |
| dma_old_wr | old | yes --dma-read-percent=0 | expected_panic | expected_panic | - | [] | - | 0/0 | 0/0 | 0/0 | system.ruby.dir_cntrl0 time: 182335 addr: 0x1042c0 event: SPM_Writeback state: M_DWR |
| dma_new | new | yes  | pass | pass | PASS | [[1591, 36], [1589, 31]] | 27537/27537, 27535/27535 | 28/32 | 28/32 | 0/0 | - |
| dma_new_rd | new | yes --dma-read-percent=100 | pass | pass | PASS | [[0, 0], [0, 0]] | 26528/26527, 26529/26529 | 68/0 | 68/0 | 2/0 | - |
| dma_new_wr | new | yes --dma-read-percent=0 | pass | pass | PASS | [[1595, 55], [1595, 46]] | 26905/26905, 26905/26905 | 0/55 | 0/55 | 0/0 | - |
| dma_new_fast | new | yes --dma-min-period=5000 --dma-max-period=50000 | pass | pass | PASS | [[1600, 85], [1600, 112]] | 110712/110711, 110707/110706 | 79/93 | 79/93 | 0/0 | - |
| dma_new_noretain | new | yes no-retain | pass | pass | PASS | [[1593, 25], [1590, 18]] | 27047/27046, 27047/27046 | 8/10 | 8/10 | 0/0 | - |
| tgt_ctrl_new | new | no  | pass | pass | PASS | [[0, 0], [0, 0]] | - | 0/0 | 0/0 | 0/0 | - |
| tgt_dma_new | new | yes  | pass | pass | PASS | [[1499, 32], [1491, 30]] | 9212/9212, 9212/9212 | 18/27 | 18/27 | 11/9 | - |
| tgt_dma_new_fast | new | yes --dma-min-period=5000 --dma-max-period=50000 | pass | pass | PASS | [[1600, 99], [1600, 108]] | 36930/36929, 36894/36893 | 92/98 | 92/98 | 39/45 | - |
| tgt_dma_new_rd_fast | new | yes --dma-min-period=5000 --dma-max-period=50000 --dma-read-percent=100 | pass | pass | PASS | [[0, 0], [0, 0]] | 34014/34012, 33875/33872 | 196/0 | 196/0 | 70/0 | - |
| tgt_dma_new_wr_fast | new | yes --dma-min-period=5000 --dma-max-period=50000 --dma-read-percent=0 | pass | pass | PASS | [[1600, 194], [1599, 181]] | 37679/37679, 37684/37683 | 0/181 | 0/181 | 0/75 | - |
| tgt_dma_old_fast | old | yes --dma-min-period=5000 --dma-max-period=50000 | expected_panic | expected_panic | - | [] | - | 0/0 | 0/0 | 0/0 | system.ruby.dir_cntrl1 time: 70610 addr: 0x2101c0 event: DMA_WRITE state: SPM_WB |

No-DMA control, old vs new binary: 20183 simulated stats equal, 0 differing, 0 missing, 0 added -> identical

## DMA requests recycled in SPM states (new binary)

- dma_new: SPM_WB.DMA_READ 20, SPM_WB.DMA_WRITE 16
- dma_new_rd: SPM_WB.DMA_READ 36
- dma_new_wr: SPM_WB.DMA_WRITE 26
- dma_new_fast: SPM_WB.DMA_READ 36, SPM_WB.DMA_WRITE 62
- dma_new_noretain: SPM_IS.DMA_READ 62, SPM_IS.DMA_WRITE 21, SPM_WB.DMA_READ 17, SPM_WB.DMA_WRITE 20
- tgt_dma_new: SPM_WB.DMA_READ 19, SPM_WB.DMA_WRITE 26
- tgt_dma_new_fast: SPM_WB.DMA_READ 54, SPM_WB.DMA_WRITE 60
- tgt_dma_new_rd_fast: SPM_WB.DMA_READ 138
- tgt_dma_new_wr_fast: SPM_WB.DMA_WRITE 131

## Retained runtime gates replayed on the new binary

| Gate | Outcome | Pass markers | Equal simulated stats | Differing | Missing | Added (all zero) |
|---|---|---|---|---|---|---|
| gate_directed_both | pass | True | 34959 | 0 | 0 | 0 (True) |
| gate_directed_fp32 | pass | True | 36188 | 0 | 0 | 0 (True) |
| gate_directed_init | pass | True | 35195 | 0 | 0 | 0 (True) |
| gate_directed_mixed | pass | True | 35470 | 0 | 0 | 0 (True) |
| gate_directed_one_entry_slow | pass | True | 34817 | 0 | 0 | 0 (True) |
| gate_directed_sync | pass | True | 34962 | 0 | 0 | 0 (True) |
| gate_smoke_normal | pass | True | 48573 | 0 | 0 | 0 (True) |
| gate_smoke_small_queues | pass | True | 48649 | 0 | 0 | 0 (True) |
| gate_spread_release_getx | pass | True | 24080 | 0 | 0 | 0 (True) |
| gate_spread_roundtrip | pass | True | 19469 | 0 | 0 | 0 (True) |
| gate_spread_roundtrip_raw | pass | True | 19463 | 0 | 0 | 0 (True) |
| gate_wide_0 | pass | True | 19649 | 0 | 0 | 0 (True) |
| gate_wide_512 | pass | True | 19649 | 0 | 0 | 0 (True) |
| gateold_spread_roundtrip | pass | True | 19469 | 0 | 0 | 0 (True) |

## Review (2026-10-13)

**Configuration.**
- The run uses the retained gate's flags, with these changes:
  - 2 cores, 2 LLC banks and 2 directories;
  - `Mesh_XY` topology, because `KVFixedMesh` rejects DMA controllers;
  - a 32 kB 4-way LLC per bank.
- Each of the two `PyTrafficGen` engines drives its own Ruby DMA controller.
- Each engine sends random full-line DMA reads and writes to one core's 16 target lines.
- The target pages were found by a `ProtocolTrace` calibration. Both guest builds land on the same pages.
- The guest (`benchmarks/spm_smoke/spm_dma_race.cpp`) loops: SPMCP, check the copy, write a new stamp through SPM, SPMWB, then a coherent load check.
- A line must always hold either the latest stamp or a uniform DMA pattern. An older stamp or a mixed line counts as an error.
- The `tgt_*` guest evicts each target line from its LLC set immediately after using it, to race clean replacements against DMA invalidations.

**Old binary: all four DMA cases panic.**
- The panic signatures cover all three bugs the model found:
  - `SPM_Writeback` in `M_DRD`/`M_DWR`;
  - `DMA_WRITE` in `SPM_WB`;
  - `CleanReplacement` in `M_DRD`, the upstream bug, in `runs_guest_v1/tgt_dma_old_fast`.
- Only the first panic in each run is observable.

**New binary: every case passes with zero data errors.**
- Every new transition fires:
  - `M_DRD`/`M_DWR` + `SPM_Writeback`: up to 196 / 181 per run;
  - the matching `*_SWB` + `Memory_Ack`;
  - `M_DRD`/`M_DWR` + `CleanReplacement`: up to 70 / 75.
- DMA requests are recycled in `SPM_IS` and `SPM_WB`. `SPM_IS` occurs only without LLC retention.
- No DMA engine stalled; each has a 1 ms progress check.
- Sent minus completed DMA packets is at most 3 at exit, which is just requests still in flight when the guest finished.

**Patch is inert without DMA.**
- The no-DMA control has identical simulated statistics on the old and new binaries.
- All 13 retained runtime gates replayed on the new binary reproduce their accepted simulated statistics exactly, with their pass markers.
- An old-binary replay of one gate confirms that the comparison is deterministic.

**Superseded first guest (`runs_guest_v1/`, `guest_v1/`).**
- It checked lines with 8 scalar loads, and initialized and restored them with scalar stores.
- A DMA write between those accesses is legal but looks torn. Three `load` errors had word 0 = the current stamp and word 7 = the DMA pattern, and appeared only in the targeted runs.
- Version 2 does every whole-line target access as one 64-byte SVE access. A contiguous 64-byte SVE access is a single gem5 memory request (`sve_mem.isa`), so it can't tear. The rebuild was otherwise unchanged.
- All v1 non-targeted DMA runs on the new binary also passed.

**Caveats.**
- This is a directed runtime test of a 2-core configuration, not the 8-core evaluation fabric.
- DMA read data is not checked by the generator. Read freshness comes from the Rumur model (VER-05).
- Partial-line DMA writes were not exercised. Upstream `M_DWR` + `Data` writes only the DMA bytes to memory and drops the LLC's dirty data for the rest of the line. This behavior is unchanged and outside this patch.
- The targeted eviction assumes the `.bss` is physically contiguous. That the clean-replacement race was reached is shown by the `CleanReplacement` transition counts.
- The patch lives only in the isolated tree `results/gem5_dma_fix_20261013/gem5`. Applying it to the main `gem5/src` was not done; it needs the user's approval.
