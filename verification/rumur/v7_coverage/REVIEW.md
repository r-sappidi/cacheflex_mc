# VER-03 manual review of unhit covers (2026-10-10)

All 13 executed cases accepted. Six instrumented coverage cases reproduce VER-02 v6
states/rules exactly (n2_s2, n2_s3, no-retention n2_s3, n2_s4, n3_s3, n2_s5), so the
346 covers (67 rule entries, 279 branches) and mutation switches add no behavior.
Union over retained-LLC cases: 242/346 covers, 59/67 rules. Saturation: n2_s4 and
n2_s5 hit the same 241 covers; n3_s3 adds one (Fwd_GETS_Silent served by an
S/SS/SM sharer) but misses two two-core deep paths, so both n3 and n2_s4+ are needed.

Unhit: 41 deliberate `error` branches (no SLICC transition / pool overflow) and 10
auto-classified config-disabled branches are expected. The 53 left for review are
classified below (line numbers are in verification/rumur/v7_coverage/private_spm.m).
The script regex missed constants containing digits (e.g. M_CAPTURE_FROM_L1), and
nested branches under disabled config are not propagated; these are category 2.

| # | Category | Covers | Lines | Status |
|---|---|---|---|---|
| 1 | Legacy source-release path (PUT/Put_Ack, PUT-wait forwards) | 21 | 529-533, 1108-1122, 1285-1289, 1443-1448 | Expected dead under RETAIN_SOURCE=1; invariant "retained-source copy never sends release PUT" holds |
| 2 | Config- or mutation-disabled (nested/regex miss) | 5 | 780, 1254, 1682, 1685, 1842 | Expected (FNF_SILENT=1, mutant off) |
| 3 | E/M L0-recall fallback for SPMCP | 11 | 807, 817 (rules), 993-1018, 1032-1041 | **Gap to resolve:** unreachable in model. With retention the recall requires !l0_present, but model L1 E/M always has L0 present (L0 E/M eviction writes back to L1 -> EE/MM). Only the S fallback (S after silent clean L0 drop) is exercised. Check SLICC: if E/M-without-L0 cannot occur there either, state the fallback as dead code; otherwise the model under-approximates. |
| 4 | Impossible by protocol | 4 | 1029, 1368, 1712, 1719 | 1029: S recall of clean L0 never returns data; 1368: LLC never grants exclusivity to a silent fetch (desired); 1712/1719: MT_I/MCT_I have one owner so pend <= 1 |
| 5 | Safety contract | 1 | 856 | Migration with no free way prevented by MIN_FREE contract (invariant "install migration never hits the no-free-way panic") |
| 6 | Single-line / single-LLC structure | 8 | 1788-1809 (L2 MEM_Inv), 1909 (dir INV for parked fetch), 1965 (error rule) | **Abstraction limit:** directory back-invalidation of the LLC never occurs; it needs a second LLC request while the line's LLC writeback is in flight, which a single-line blocking LLC cannot issue. Directory recall during silent fetch or SPMWB is therefore not exercised. |
| 7 | RETAIN_LLC=0 only | 2 | 1928, 1980 | Hit 1610x each in cov_no_retention_n2_s3 |
| 8 | Baseline MESI branch not reached | 1 | 1150 | Requester-initiated (non-LLC) Inv to an SS sharer; not SPM-specific, unreached through n2_s5/n3_s3 |

## Mutation testing

| Mutant | Outcome | Detected by |
|---|---|---|
| Silent copier registered as sharer | survived n2_s3 (identical 735612/3731573); caught n3_s3 | silent request changed coherent registration |
| LLC answers silent copy from stale data when owner exists | caught n2_s3 | SPMCP snapshot missed the latest coherent value |
| Retained snapshot from L1 ignoring newer L0 | caught n2_s3 | same snapshot assertion |
| SPMWB commits before invalidation acks | caught n2_s3 | invariant "shared copies fresh" |
| Silent NACK drops retry | caught n2_s3 | liveness "all cores can drain" |
| SS L1 acks any invalidation keeping its copy | caught n2_s3 | invariant "SWMR" |

The survivor at n2_s3 is structural: with two cores an invalid copier under LLC SS
must be a stale silent-drop sharer, so a non-sharer silent copy needs three cores.
Silent-registers detection is by a direct registration assertion; the other five
are detected by semantic properties (freshness, SWMR, liveness).

## Remaining work for the letter

1. Category 3: inspect SLICC L0/L1 for E/M-without-L0 reachability; either document
   dead code or extend the model so the E/M recall fallback is exercised.
2. Category 6: either state the limit or add a directory-initiated LLC recall
   environment rule (would also need a fresh v6-equivalence break, i.e. new bounds).
3. Optional: instrumented RETAIN_SOURCE=0 run to show category 1 is reachable when enabled.

## Addendum 2026-10-11: category 3 resolved by SLICC reading (exploratory check)

SLICC: L1 E/M means "in L1 and L0"; a non-snapshot SPMCP_FETCH (L0 line absent)
reaches L1 E/M only if it overtakes L0's eviction PUTX. L0 sends SPMCP on
`requestNetwork_out` (bufferToL1) and PUTX on `replyNetwork_out` (replyToL1).
Legacy configs alias replyToL1 to bufferToL1 (one FIFO), so overtaking is impossible
and the model's `l0q_cnt[c] = 0` recall guard is exact. The finite profile
(`scripts/finite_micro_se.py`) wires replyToL1 to a separate ordered
`l1.returnBufferFromL0`, so a fetch can overtake a back-pressured PUTX or one with
larger latency than `spm_copy_request_latency` (set to 1 in calibrated
sensitivities). That is the race the SLICC "F2 fix" ({EX_L0,MX_L0}, WriteBack ->
MX_L0) handles, so the v6/v7 model under-approximates the finite profile.

Exploratory scratch model (not formal evidence): v7 plus FINITE_RETURN_LANE=1
removing `l0q_cnt[c] = 0` from the S/E/M SPMCP recall guards; instrumented, same
tool/flags, 2 threads, broker lease formal_scratch_finite_lane (6 GiB, released).
n2_s3 739116/3744661, n3_s3 8218655/50726832, n2_s4 7164818/37507870: no error
trace, no liveness violation, errors equal unhit covers. The E/M recall rules, WB
absorption in EX_L0/MX_L0 (F2) and IACK completion to EE/MM are now reached.
Still unhit: L0 DataAck in SX/EX/MX_L0, which is dead under retention because the
recall is issued only when L0 is absent (dirty data arrives via the overtaken WB).
A formal v8 queue (this switch, mutants re-run, plus n2_s5) is needed before citing it.
