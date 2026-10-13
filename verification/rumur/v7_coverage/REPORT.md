# VER-03 coverage and mutation report

Queue stage: complete_requires_review

| Case | Outcome | States / rules | v6 reference | Covers hit | Rules hit | Unhit: error / config / review |
|---|---|---|---|---|---|---|
| v7_off_n2_s3 | equivalent_pass | [735612, 3731573] | [735612, 3731573] | -/- | -/- | - / - / - |
| cov_n2_s2 | equivalent_pass | [43371, 196086] | [43371, 196086] | 207/346 | 55/67 | 41 / 10 / 88 |
| cov_n2_s3 | equivalent_pass | [735612, 3731573] | [735612, 3731573] | 239/346 | 59/67 | 41 / 10 / 56 |
| cov_no_retention_n2_s3 | equivalent_pass | [719825, 3669354] | [719825, 3669354] | 241/346 | 59/67 | 41 / 10 / 54 |
| cov_n2_s4 | equivalent_pass | [7118218, 37322334] | [7118218, 37322334] | 241/346 | 59/67 | 41 / 10 / 54 |
| cov_n3_s3 | equivalent_pass | [8200271, 50638416] | [8200271, 50638416] | 240/346 | 59/67 | 41 / 10 / 55 |
| cov_n2_s5 | equivalent_pass | [48275746, 250746538] | [48275746, 250746538] | 241/346 | 59/67 | 41 / 10 / 54 |

## Mutants

| Case | Outcome | Detected by | States at detection |
|---|---|---|---|
| mut_m_silent_registers_n2_s3 | mutant_survived | - | [735612, 3731573] |
| mut_m_silent_registers_n3_s3 | mutant_caught | silent request changed coherent registration | [195329, 896581] |
| mut_m_silent_stale_owner_n2_s3 | mutant_caught | SPMCP snapshot missed the latest coherent value | [39621, 134559] |
| mut_m_capture_from_l1_n2_s3 | mutant_caught | SPMCP snapshot missed the latest coherent value | [14958, 47149] |
| mut_m_early_wb_commit_n2_s3 | mutant_caught | invariant "shared copies fresh" failed; shared copies fresh | [90835, 388824] |
| mut_m_retry_dropped_n2_s3 | mutant_caught | all cores can drain | [735612, 3727677] |
| mut_m_inv_keeps_copy_n2_s3 | mutant_caught | SWMR; invariant "SWMR" failed | [224126, 1013089] |

## Union over retained cases (cov_n2_s2, cov_n2_s3, cov_n2_s4, cov_n3_s3, cov_n2_s5)

Covers hit: 242/346; rule entries hit: 59/67

First bound reaching each cover (hit only at larger bounds):

- cov_n2_s3: L1 Load hit S/SS L695 (rule); L1 Load hit S/SS L699 (true); L1 Store S/SS -> SM (UPGRADE) L746 (rule); L1 SPMCP_fetch S -> SX_L0 (recall L0) L797 (rule); L1 handles L0 queue head L1007 (l1s[c] = L1_SM_IL0); L1 handles L0 queue head L1008 (l1s[c] = L1_SX_L0); L1 fwd triggers L0_Invalidate_Else L1096 (else); L1 handles Inv L1143 (else); L1 handles Inv L1146 (else); L1 handles Inv L1158 (else); L1 handles Inv L1160 (l1s[c] = L1_IM); L1 handles Inv L1161 (rl2); L1 handles Inv L1169 (else); L1 handles DATA L1335 (else); L1 handles ACK L1382 (rule); L1 handles ACK L1385 (l1s[c] = L1_SM); L1 handles ACK L1386 (l1t_pend[c] - a = 0); L1 handles ACK L1391 (else); L1 handles ACK L1394 (l1s[c] = L1_IM); L1 handles ACK L1397 (else); L2 handles request L1535 (l2s = L2_SS); L2 handles request L1556 (l2s = L2_SS); L2 handles request L1559 (l2_sharer[c]); L2 handles request L1562 (l2_sharer[o] & o != c); L2 handles request L1577 (req[i].t = RQ_UPG); L2 handles request L1580 (l2s = L2_SS); L2 handles request L1584 (l2_sharer[o] & o != c); L2 handles request L1601 (l2s = L2_SS); L2 handles request L1624 (l2s = L2_SS | l2s = L2_MT); L2 handles request L1629 (l2_sharer[o]); L2 handles response L1654 (l2s = L2_SPM_WB_INV & (t = R2_ACK | t = R2_WBDATA)); L2 handles response L1657 (l2t_pend = 0)
- cov_n2_s4: L1 handles Inv L1162 (else); L1 handles DATA L1324 (fl1)
- cov_n3_s3: L1 handles Fwd_GETS_Silent L1240 (FNF_SILENT = 1 & (l1s[c] = L1_S | l1s[c] = L1_SS | l1s[c] = L1_SM))

### Unhit — review (53)

- `proc:relinquish_source` L529 then: `l1s[c]=L1_SS` -> `l1s[c]:=L1_SX_A;push_req(RQ_PUTS,c,l1_data[c],false);`
- `proc:relinquish_source` L531 then: `l1s[c]=L1_EE` -> `l1s[c]:=L1_EX_A;push_req(RQ_PUTE,c,l1_data[c],false);`
- `proc:relinquish_source` L533 else: `else after l1s[c]=L1_SS | l1s[c]=L1_EE` -> `assert l1s[c]=L1_MM;`
- `L0 captures retained coherent snapshot` L780 then: `M_CAPTURE_FROM_L1=1` -> `capture_value[c]:=l1_data[c]; else capture_value[c]:=l0_data[c]; endif;`
- `L1 SPMCP_fetch E -> EX_L0 (recall L0)` L807 rule: `else after ` -> ``
- `L1 SPMCP_fetch M -> MX_L0 (recall L0)` L817 rule: `else after ` -> ``
- `L1 SPMCP_install (displaces + migrates coherent line)` L856 else: `else after free_ways(c) >= 1` -> `no_free_way := true;`
- `L1 handles L0 queue head` L993 then: `IMPLEMENTED_TRANSITIONS = 1 & (l1s[c] = L1_MX_L0 | l1s[c] = L1_EX_L0)` -> `if d then l1_data[c] := v; l1_dirty[c] := true; endif;`
- `L1 handles L0 queue head` L995 then: `d` -> `l1_data[c] := v; l1_dirty[c] := true; endif;`
- `L1 handles L0 queue head` L1010 then: `l1s[c] = L1_EX_L0` -> `if IMPLEMENTED_TRANSITIONS = 1 & l1_dirty[c] then`
- `L1 handles L0 queue head` L1012 then: `IMPLEMENTED_TRANSITIONS = 1 & l1_dirty[c]` -> `l1s[c] := L1_MM;`
- `L1 handles L0 queue head` L1014 else: `else after IMPLEMENTED_TRANSITIONS = 1 & l1_dirty[c]` -> `l1s[c] := L1_EE;`
- `L1 handles L0 queue head` L1018 then: `l1s[c] = L1_MX_L0` -> `l1s[c] := L1_MM; relinquish_source(c);`
- `L1 handles L0 queue head` L1029 then: `l1s[c] = L1_SX_L0` -> `l1_data[c] := v; l1_dirty[c] := true;`
- `L1 handles L0 queue head` L1032 then: `l1s[c] = L1_EX_L0` -> `l1_data[c] := v; l1_dirty[c] := true;`
- `L1 handles L0 queue head` L1035 then: `IMPLEMENTED_TRANSITIONS = 1` -> `l1s[c] := L1_MM;`
- `L1 handles L0 queue head` L1041 then: `l1s[c] = L1_MX_L0` -> `l1_data[c] := v; l1_dirty[c] := true;`
- `forward while local SPM PUT is pending` L1108 rule: `else after ` -> ``
- `forward while local SPM PUT is pending` L1111 then: `t=FW_SILENT` -> `if l1s[c]=L1_IX_A then push_rspc(rq,RC_RETRY,0,false,0,true);`
- `forward while local SPM PUT is pending` L1112 then: `l1s[c]=L1_IX_A` -> `push_rspc(rq,RC_RETRY,0,false,0,true);`
- `forward while local SPM PUT is pending` L1113 else: `else after l1s[c]=L1_IX_A` -> `push_rspc(rq,RC_DATA,l1_data[c],false,0,true);endif;`
- `forward while local SPM PUT is pending` L1114 then: `t=FW_GETS` -> `push_rspc(rq,RC_DATA,l1_data[c],l1_dirty[c],0,true);`
- `forward while local SPM PUT is pending` L1117 else: `else after t=FW_SILENT | t=FW_GETS` -> `if l1s[c]!=L1_IX_A then l1t_data[c]:=l1_data[c];endif;`
- `forward while local SPM PUT is pending` L1118 then: `l1s[c]!=L1_IX_A` -> `l1t_data[c]:=l1_data[c];endif;`
- `forward while local SPM PUT is pending` L1119 then: `t=FW_GETX` -> `push_rspc(rq,RC_DATA,l1t_data[c],l1_dirty[c],0,true);`
- `forward while local SPM PUT is pending` L1120 then: `l1s[c]=L1_EX_A | l1s[c]=L1_MX_A` -> `push_rsp2(R2_WBDATA,c,l1t_data[c],l1_dirty[c]);`
- `forward while local SPM PUT is pending` L1121 then: `rl2` -> `push_rsp2(R2_ACK,c,0,false);`
- `forward while local SPM PUT is pending` L1122 else: `else after t=FW_GETX | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | rl2` -> `push_rspc(rq,RC_ACK,0,false,1,true);endif;`
- `L1 handles Inv` L1150 else: `else after rl2` -> `push_rspc(rq, RC_ACK, 0, false, 1, true); endif;`
- `L1 handles Fwd_GETS_Silent` L1254 then: `IMPLEMENTED_TRANSITIONS = 1` -> `push_rsp2(R2_WBDATA, c, l1t_data[c], l1t_dirty[c]);`
- `SPM PUT acknowledgment installs saved snapshot` L1285 rule: `else after ` -> ``
- `SPM PUT acknowledgment installs saved snapshot` L1288 then: `l1s[c]=L1_IX_A` -> `spm_install(c,l1t_data[c]);`
- `SPM PUT acknowledgment installs saved snapshot` L1289 else: `else after l1s[c]=L1_IX_A` -> `spm_install(c,l1_data[c]);endif;`
- `L1 handles DATA_EXCLUSIVE` L1368 then: `l1s[c] = L1_IX_D` -> `l1s[c] := L1_I;`
- `L2 accepts local-source SPM PUT` L1443 rule: `else after ` -> ``
- `L2 accepts local-source SPM PUT` L1445 then: `l2_sharer[c]` -> `if req[i].t=RQ_PUTM then l2_data:=req[i].val;l2_dirty:=true;endif;`
- `L2 accepts local-source SPM PUT` L1446 then: `req[i].t=RQ_PUTM` -> `l2_data:=req[i].val;l2_dirty:=true;endif;`
- `L2 accepts local-source SPM PUT` L1448 then: `l2s=L2_MT` -> `l2_excl:=NCORE;l2s:=L2_M;endif;`
- `L2 handles response` L1682 then: `IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_MT_SPMS` -> `l2_data := v;`
- `L2 handles response` L1685 then: `d` -> `l2_dirty := true; endif;`
- `L2 handles response` L1712 else: `else after l2t_pend = 1` -> `l2t_pend := l2t_pend - 1;`
- `L2 handles response` L1719 else: `else after l2t_pend = 1` -> `l2t_pend := l2t_pend - 1;`
- `L2 handles MEM_Inv` L1788 rule: `else after ` -> ``
- `L2 handles MEM_Inv` L1790 then: `l2s = L2_SS` -> `l2_clear_tbe();`
- `L2 handles MEM_Inv` L1795 then: `l2_sharer[o]` -> `push_fwd(o, FW_INV, true, 0); endif;`
- `L2 handles MEM_Inv` L1799 then: `l2s = L2_M` -> `l2_clear_tbe();`
- `L2 handles MEM_Inv` L1804 then: `l2s = L2_MT` -> `l2_clear_tbe();`
- `L2 handles MEM_Inv` L1809 then: `l2_sharer[o]` -> `push_fwd(o, FW_INV, true, 0); endif;`
- `L2 handles unblock` L1842 then: `l2s = L2_MT_SPMS` -> `l2s := L2_MT;`
- `dir M sends INV for parked fetch` L1909 rule: `else after ` -> ``
- `dir handles request` L1928 then: `t = DQ_SILENT` -> `memq := MQ_READ; memq_val := 0;`
- `dir CleanReplacement undefined in transients` L1965 rule: `else after ` -> ``
- `memory completes` L1980 then: `dirs = D_SPM_IS` -> `push_rsp2(R2_MEMDATA, 0, mem_data, false);`

### Unhit — config_disabled (10)

- `proc:relinquish_source` L527 else: `else after RETAIN_SOURCE=1` -> `cpu_busy[c]:=true;`
- `L1 handles L0 queue head` L1037 else: `else after IMPLEMENTED_TRANSITIONS = 1` -> `l1s[c] := L1_EE;`
- `L1 handles Fwd_GETS_Silent` L1238 then: `FNF_SILENT = 0` -> `push_unb(UB_UNB, c); endif;`
- `L1 handles Fwd_GETS_Silent` L1257 else: `else after FNF_SILENT = 1 | IMPLEMENTED_TRANSITIONS = 1` -> `push_unb(UB_UNB, c);`
- `L2 handles request` L1599 else: `else after RETAIN_LLC=1` -> `push_dreq(DQ_SILENT,0);endif;`
- `L2 handles request` L1604 then: `M_SILENT_REGISTERS=1` -> `l2_sharer[c] := true; endif;`
- `L2 handles request` L1607 then: `l2s = L2_MT & M_SILENT_STALE_OWNER=1` -> `push_rspc(c, RC_DATA, l2_data, false, 0, false);`
- `L2 handles request` L1611 then: `FNF_SILENT = 0` -> `l2s := L2_MT_SPMS;`
- `L2 handles request` L1632 then: `M_EARLY_WB_COMMIT=1` -> `l2t_pend := 0; commit_spm_writeback(); endif;`
- `L2 handles response` L1754 else: `else after RETAIN_LLC=1` -> `l2_dealloc();l2s:=L2_NP;endif;`

### Unhit — model_error_branch (41)

- `proc:push_req` L284 then: `!done` -> `error "request pool overflow (raise REQ_POOL)"; endif;`
- `proc:push_fwd` L304 then: `!done` -> `error "forward pool overflow (raise FWD_POOL)"; endif;`
- `proc:push_rspc` L325 then: `!done` -> `error "core response pool overflow (raise RSPC_POOL)"; endif;`
- `proc:push_rsp2` L345 then: `!done` -> `error "L2 response pool overflow (raise RSP2_POOL)"; endif;`
- `proc:push_unb` L363 then: `!done` -> `error "unblock pool overflow (raise UNB_POOL)"; endif;`
- `proc:push_dreq` L383 then: `!done` -> `error "dir request pool overflow (raise DREQ_POOL)"; endif;`
- `proc:push_d2d` L402 then: `!done` -> `error "L2->dir response pool overflow (raise D2D_POOL)"; endif;`
- `proc:l0q_push` L407 then: `l0q_cnt[c] = L0Q_DEPTH` -> `error "L0->L1 queue overflow (raise L0Q_DEPTH)";`
- `L1 handles L0 queue head` L996 else: `else after l1s[c] = L1_M | l1s[c] = L1_E | l1s[c] = L1_M_IL0 | l1s[c] = L1_E_IL0 | IMPLEMENTED_TRANSITIONS = 1 & (l1s[c] = L1_MX_L0 | l1s[c] = L1_EX_L0)` -> `error "L1: WriteBack from L0 in a state with no SLICC transition";`
- `L1 handles L0 queue head` L1020 else: `else after l1s[c] = L1_S_IL0 | l1s[c] = L1_E_IL0 | l1s[c] = L1_M_IL0 | l1s[c] = L1_MM_IL0 | l1s[c] = L1_SM_IL0 | l1s[c] = L1_SX_L0 | l1s[c] = L1_EX_L0 | l1s[c] = L1_MX_L0` -> `error "L1: L0_Ack in a state with no SLICC transition";`
- `L1 handles L0 queue head` L1044 else: `else after l1s[c] = L1_M_IL0 | l1s[c] = L1_E_IL0 | l1s[c] = L1_SX_L0 | l1s[c] = L1_EX_L0 | l1s[c] = L1_MX_L0` -> `error "L1: L0_DataAck in a state with no SLICC transition";`
- `L1 handles Inv` L1170 else: `else after l1s[c] = L1_I | l1s[c] = L1_IX_D | l1s[c] = L1_SS | l1s[c] = L1_EE | l1s[c] = L1_MM | l1s[c] = L1_IS | l1s[c] = L1_IS_I | l1s[c] = L1_IM | l1s[c] = L1_M_I | l1s[c] = L1_SINK` -> `error "L1: Inv in a state with no SLICC transition";`
- `L1 handles Fwd_GETX` L1192 else: `else after l1s[c] = L1_EE | l1s[c] = L1_MM | l1s[c] = L1_M_I` -> `error "L1: Fwd_GETX in a state with no SLICC transition";`
- `L1 handles Fwd_GETS` L1216 else: `else after l1s[c] = L1_EE | l1s[c] = L1_MM | l1s[c] = L1_M_I` -> `error "L1: Fwd_GETS in a state with no SLICC transition";`
- `L1 handles Fwd_GETS_Silent` L1261 else: `else after l1s[c] = L1_MM | l1s[c] = L1_EE | FNF_SILENT = 1 & (l1s[c] = L1_S | l1s[c] = L1_SS | l1s[c] = L1_SM) | FNF_SILENT = 1 & (l1s[c] = L1_I | l1s[c] = L1_IS | l1s[c] = L1_IM | l1s[c] = L1_IS_I) | l1s[c] = L1_M_I` -> `error "L1: Fwd_GETS_Silent in a state with no SLICC transition";`
- `L1 handles DATA` L1313 then: `!fl1 & a != 0` -> `error "L1: IS data with outstanding acks is not a SLICC path";`
- `L1 handles DATA` L1347 else: `else after l1s[c] = L1_IS | l1s[c] = L1_IS_I | l1s[c] = L1_IM | l1s[c] = L1_IX_D` -> `error "L1: DATA in a state with no SLICC transition";`
- `L1 handles DATA_EXCLUSIVE` L1373 else: `else after l1s[c] = L1_IS | l1s[c] = L1_IS_I | l1s[c] = L1_IX_D` -> `error "L1: DATA_EXCLUSIVE in a state with no SLICC transition";`
- `L1 handles ACK` L1395 then: `l1t_pend[c] - a = 0` -> `error "L1: IM Ack_all has no SLICC transition";`
- `L1 handles ACK` L1400 else: `else after l1s[c] = L1_SM | l1s[c] = L1_IM` -> `error "L1: ACK in a state with no SLICC transition";`
- `L1 handles WB_Ack` L1416 else: `else after l1s[c] = L1_M_I | l1s[c] = L1_SINK` -> `error "L1: WB_Ack in a state with no SLICC transition";`
- `L1 handles SPMWB_Ack` L1429 else: `else after spm_st[c] = SP_XWB` -> `error "L1: SPMWB_Ack with slot not in XWB has no SLICC transition";`
- `L2 handles request` L1506 else: `else after l2s = L2_MT | l2s = L2_NP | l2s = L2_IS | l2s = L2_ISS | l2s = L2_IM | l2s = L2_SS | l2s = L2_M | l2s = L2_M_I | l2s = L2_I_I | l2s = L2_S_I | l2s = L2_MT_IB | l2s = L2_MT_SB | (IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_SPM_IS)` -> `error "L2: L1_PUTX in a state with no SLICC transition";`
- `L2 handles request` L1517 else: `else after l2s = L2_NP | l2s = L2_SS | l2s = L2_M | l2s = L2_MT | l2s = L2_M_I | l2s = L2_I_I | l2s = L2_S_I | l2s = L2_IS | l2s = L2_ISS | l2s = L2_IM | l2s = L2_MT_IB | l2s = L2_MT_SB | (IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_SPM_IS)` -> `error "L2: L1_PUTX_old in a state with no SLICC transition";`
- `L2 handles request` L1544 else: `else after l2s = L2_NP | l2s = L2_IS | l2s = L2_ISS | l2s = L2_SS | l2s = L2_M | l2s = L2_MT` -> `error "L2: L1_GETS in a state with no SLICC transition";`
- `L2 handles request` L1573 else: `else after l2s = L2_NP | l2s = L2_SS | l2s = L2_M | l2s = L2_MT` -> `error "L2: L1_GETX in a state with no SLICC transition";`
- `L2 handles request` L1589 else: `else after l2s = L2_SS` -> `error "L2: L1_UPGRADE in a state with no SLICC transition";`
- `L2 handles request` L1614 else: `else after l2s = L2_NP | l2s = L2_SS | l2s = L2_M | l2s = L2_MT & M_SILENT_STALE_OWNER=1 | l2s = L2_MT` -> `error "L2: SPM_GETS_SILENT in a state with no SLICC transition";`
- `L2 handles request` L1633 else: `else after l2s = L2_NP | l2s = L2_M | WB_INVALIDATE=0 | l2s = L2_SS | l2s = L2_MT` -> `error "SPM writeback accepted in invalid state";`
- `L2 handles response` L1688 else: `else after l2s = L2_MT_IIB | l2s = L2_MT_IB | l2s = L2_MT_I | l2s = L2_MCT_I | IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_MT_SPMS` -> `error "L2: WB_Data in a state with no SLICC transition";`
- `L2 handles response` L1722 else: `else after l2s = L2_I_I | l2s = L2_S_I | l2s = L2_MT_I | l2s = L2_MCT_I` -> `error "L2: Ack in a state with no SLICC transition";`
- `L2 handles response` L1756 else: `else after l2s = L2_ISS | l2s = L2_IS | l2s = L2_IM | l2s = L2_SPM_IS` -> `error "L2: Mem_Data in a state with no SLICC transition";`
- `L2 handles response` L1764 else: `else after l2s = L2_M_I` -> `error "L2: Mem_Ack in a state with no SLICC transition";`
- `L2 handles response` L1774 else: `else after l2s = L2_SPM_WB` -> `error "L2: SPMWB_Ack in a state with no SLICC transition";`
- `L2 handles unblock` L1832 else: `else after l2s = L2_SS_MB | l2s = L2_MT_MB` -> `error "L2: Exclusive_Unblock in a state with no SLICC transition";`
- `L2 handles unblock` L1845 else: `else after l2s = L2_MT_IIB | l2s = L2_MT_SB | l2s = L2_MT_SPMS` -> `error "L2: Unblock in a state with no SLICC transition";`
- `dir M sends INV for parked fetch` L1911 then: `dir_inv = 2` -> `error "dir INV overflow"; endif;`
- `dir handles request` L1924 then: `memq != MQ_NONE` -> `error "memory queue overflow"; endif;`
- `dir handles L2 response` L1951 then: `memq != MQ_NONE` -> `error "memory queue overflow"; endif;`
- `memory completes` L1983 else: `else after dirs = D_IM | dirs = D_SPM_IS` -> `error "dir: Memory_Data in a state with no SLICC transition";`
- `memory completes` L1996 else: `else after dirs = D_MI | dirs = D_SPM_WB` -> `error "dir: Memory_Ack in a state with no SLICC transition";`
