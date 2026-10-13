# VER-05 review: DMA at the directory (2026-10-11)

Queue stage `complete_requires_review`; 19/19 cases accepted. Model: `verification/rumur/v9_dma/private_spm.m`
(v8 finite return lane + one DMA agent). Fix switches: DMA_STALL_SPM=1, SWB_IN_DRD=2, CLEAN_FIX=1.

## Control
- ctrl_nodma_n2_s3 (DMA_OPS=0): 739116 / 3744661, identical to VER-04 plain_n2_s3.

## Implemented SLICC behaviour (each a counterexample at n2_s3, DMA_OPS=1)
| Case | First violation | States at detection |
|---|---|---|
| bug_spm_state_dma | DMA request in dir SPM_IS/SPM_WB: no SLICC transition | 10204 |
| bug_spmwb_in_drd (DMA stall only) | SPM_Writeback arriving in M_DRD/M_DWR: no SLICC transition | 336480 |
| bug_upstream_clean | CleanReplacement in M_DRD/M_DWR: no transition (upstream MESI_Two_Level-dir) | 1320 |
| naive_stall (stall writeback in M_DRD) | deadlock: "all cores can drain" fails (LLC SPM_WB recycles INV; dir waits for data) | 7330997 |

## Fixed model: no violation
| Case | States / rules |
|---|---|
| fix_dma1_n2_s2 | 417307 / 2115035 |
| fix_dma1_n2_s3 | 7331433 / 41102077 |
| fix_dma1_no_retention_n2_s3 | 7003481 / 39661794 |
| fix_dma2_n2_s3 (2 DMA ops) | 24331733 / 141307752 |
| fix_dma1_n2_s4 | 71687743 / 414112734 |
| fix_dma1_n3_s3 | 84205495 / 563844149 |

## Mutants on the fixed model
5/6 were caught at n2_s3. M_SILENT_REGISTERS survived n2_s3 (expected: it needs a third core, as in VER-03/04), and escalation caught it at n3_s3.

## Coverage (cov_fix_dma1_n2_s3)
- Instrumented counts equal the plain counts (7331433 / 41102077).
- 282/382 covers and 68/73 rules hit.
- Every DMA rule and every fix branch was hit:
  - SPM_Writeback accepted in M_DRD and M_DWR: 218 each;
  - DMA write serialized while an SPMWB is in flight: 222;
  - DMA memory read ordered before an in-flight SPMWB: 372;
  - CleanReplacement during a DMA: 42028 each.
- "L2 handles MEM_Inv" (directory back-invalidation of the LLC) is hit 1505855 times. It was unhit in every VER-03/04 case, so the VER-03 open item is closed under DMA.
- Unhit DMA-area covers are model error branches (queue overflow and the no-transition errors disabled by the fix switches), plus:
  - **`dir CleanReplacement undefined in transients`:** an error rule. Not reaching it means a clean eviction reaches the directory only in M, I, M_DRD or M_DWR.
  - **`memory completes` / `dirs = D_SPM_IS`:** reachable only without LLC retention. It was hit in VER-04 cov_no_retention_n2_s3 and is unchanged by DMA.
- The other unhit rules match VER-04 (local-source SPM PUT paths and the parked-fetch INV).

## Caveats
- One DMA agent, one line, bounded steps; this is not a proof.
- DMA is modeled at the directory only. Device-side DMA controller ordering is not modeled.
- Proposed SLICC patch `verification/rumur/v9_dma/dir_dma_fix.proposed.patch`:
  - passes `git apply --check`, and SLICC generation succeeds;
  - not applied, not compiled into gem5, and not runtime-tested;
  - no SE-mode configuration has DMA devices, so existing runs are unaffected.
