-- v9 DMA copy of v8 (2026-10-11); v8 finite-return-lane copy of v7 (2026-10-11); v7 coverage/mutation copy of verification/rumur/current/private_spm.m (2026-10-10).
-- Current retained-source snapshot + silent-miss abstraction, 2026-10-01. See README and SOURCE_MAP.
-- One coherent line, one outstanding CPU op/core; bounded unordered message pools.
-- Retry enabled, coherent writeback invalidations, transient migration allowed.
-- Not a finite-network or speculative CPU model. Historical model preserved one level up.
const
  RETAIN_SOURCE: 1;
  -- v8: 1 = finite profile, where a non-snapshot SPMCP_FETCH (bufferToL1) may overtake
  -- L0 eviction/recall returns queued on the separate ordered return lane
  -- (scripts/finite_micro_se.py). 0 = legacy single L0->L1 FIFO (VER-02/03 model).
  FINITE_RETURN_LANE: 1;
  -- v9 DMA agent at the directory (MESI_Three_Level_SPM-dir.sm DMA_READ/DMA_WRITE).
  -- DMA_OPS: DMA operations per run (0 = v8 model).
  -- DMA_STALL_SPM: 0 = implemented SLICC (no DMA transition in SPM_IS/SPM_WB);
  --   1 = fix: recycle DMA there like other transients.
  -- SWB_IN_DRD: fresh requests reaching M_DRD/M_DWR. 0 = implemented SLICC (no
  --   transition); 1 = naive stall of everything; 2 = fix: accept SPM_Writeback
  --   (write memory, ack LLC, keep waiting for INV data) and stall fetches.
  DMA_OPS: 0;
  DMA_STALL_SPM: 0;
  SWB_IN_DRD: 0;
  -- CLEAN_FIX: 1 = proposed fix for an upstream MESI_Two_Level-dir race (not SPM-specific):
  -- CleanReplacement arriving in M_DRD/M_DWR acks the LLC and serves the DMA from memory.
  CLEAN_FIX: 0;
  -- v7 mutation switches (0 = implemented protocol). Each 1 breaks one mechanism.
  M_SILENT_REGISTERS: 0;   -- LLC records silent copier as a sharer
  M_SILENT_STALE_OWNER: 0; -- LLC answers silent copy from stale data instead of forwarding to owner
  M_CAPTURE_FROM_L1: 0;    -- retained snapshot taken from L1 copy, ignoring newer L0 data
  M_EARLY_WB_COMMIT: 0;    -- SPMWB commits before coherent invalidations are acknowledged
  M_RETRY_DROPPED: 0;      -- silent NACK consumed without retaining a retry
  M_INV_KEEPS_COPY: 0;     -- SS L1 acknowledges any invalidation without dropping its copy
  WB_INVALIDATE: 1;
  RETAIN_LLC: 1;

  IMPLEMENTED_TRANSITIONS: 1;

  FNF_SILENT: 1;

  INSTALL_STABLE_ONLY: 0;

  STEPS:        3;

  NCORE:      3;
  VMAX:       1;

  WAYS:       3;
  MAXPIN:     2;

  MIN_FREE:   2;
  REQ_POOL:   6;
  FWD_POOL:   4;

  RSPC_POOL:  4;
  RSP2_POOL:  5;
  UNB_POOL:   3;
  DREQ_POOL:  2;
  D2D_POOL:   2;
  L0Q_DEPTH:  2;

type
  CoreT:     0 .. NCORE-1;
  MaybeCore: 0 .. NCORE;
  ValT:      0 .. VMAX;
  AckT:      -NCORE .. NCORE;
  PendT:     0 .. NCORE;

  ReqIdx:  0 .. REQ_POOL-1;
  FwdIdx:  0 .. FWD_POOL-1;
  RspCIdx: 0 .. RSPC_POOL-1;
  Rsp2Idx: 0 .. RSP2_POOL-1;
  UnbIdx:  0 .. UNB_POOL-1;
  DReqIdx: 0 .. DREQ_POOL-1;
  D2DIdx:  0 .. D2D_POOL-1;
  L0QIdx:  0 .. L0Q_DEPTH-1;
  L0QCnt:  0 .. L0Q_DEPTH;

  L1StateT: enum {
    L1_I, L1_S, L1_SS, L1_E, L1_EE, L1_M, L1_MM,
    L1_IS, L1_IM, L1_SM, L1_IS_I, L1_M_I, L1_SINK,
    L1_S_IL0, L1_E_IL0, L1_M_IL0, L1_MM_IL0, L1_SM_IL0,
    L1_SX_A, L1_EX_A, L1_MX_A, L1_IX_A, L1_IX_D, L1_SX_L0, L1_EX_L0, L1_MX_L0
  };

  SlotT: enum { SP_INV, SP_X, SP_XWB };

  L2StateT: enum {
    L2_NP, L2_ISS, L2_IS, L2_IM, L2_SPM_IS, L2_SPM_WB, L2_SPM_WB_INV,
    L2_SS, L2_M, L2_MT,
    L2_SS_MB, L2_MT_MB, L2_MT_IIB, L2_MT_IB, L2_MT_SB, L2_MT_SPMS,
    L2_M_I, L2_MT_I, L2_MCT_I, L2_I_I, L2_S_I
  };

  DirStateT: enum { D_I, D_IM, D_M, D_MI, D_SPM_IS, D_SPM_WB,
                    D_ID, D_IDW, D_MDRD, D_MDRDI, D_MDWR, D_MDWRI, D_MDRD_SWB, D_MDWR_SWB };
  DmaReqT: enum { DMA_NONE, DMA_RD, DMA_WR };

  MemOpT: enum { MQ_NONE, MQ_READ, MQ_WB };

  CpuOpT: enum { OP_NONE, OP_LD, OP_ST, OP_CPF, OP_INST,
                 OP_SLD, OP_SST, OP_SWB, OP_REL };

  ReqTypeT: enum { RQ_GETS, RQ_GETX, RQ_UPG, RQ_PUTX, RQ_SILENT, RQ_PUTS, RQ_PUTE, RQ_PUTM, RQ_SPMWB };
  ReqMsgT: record
    valid: boolean;
    t:     ReqTypeT;
    src:   CoreT;
    val:   ValT;
    dirty: boolean;
  end;

  FwdTypeT: enum { FW_INV, FW_GETX, FW_GETS, FW_SILENT };
  FwdMsgT: record
    valid:     boolean;
    t:         FwdTypeT;
    req_is_l2: boolean;
    req:       CoreT;
  end;

  RspCTypeT: enum { RC_DATA, RC_DATAX, RC_ACK, RC_RETRY, RC_PUTACK, RC_WBACK, RC_SPMWBACK };
  RspCMsgT: record
    valid:   boolean;
    t:       RspCTypeT;
    val:     ValT;
    dirty:   boolean;
    ack:     AckT;
    from_l1: boolean;
  end;

  Rsp2TypeT: enum { R2_WBDATA, R2_ACK, R2_MEMDATA, R2_MEMACK, R2_SPMWBACK };
  Rsp2MsgT: record
    valid: boolean;
    t:     Rsp2TypeT;
    src:   CoreT;
    val:   ValT;
    dirty: boolean;
  end;

  UnbTypeT: enum { UB_UNB, UB_XUNB };
  UnbMsgT: record
    valid: boolean;
    t:     UnbTypeT;
    src:   CoreT;
  end;

  DReqTypeT: enum { DQ_GETS, DQ_SILENT, DQ_SPMWB };
  DReqMsgT: record
    valid:    boolean;
    t:        DReqTypeT;
    val:      ValT;
    inv_sent: boolean;
  end;

  D2DTypeT: enum { DD_DATA, DD_ACK };
  D2DMsgT: record
    valid: boolean;
    t:     D2DTypeT;
    val:   ValT;
    dirty: boolean;
  end;

  L0MsgKindT: enum { L0M_WB, L0M_IACK, L0M_IDATA };
  L0MsgT: record
    kind:  L0MsgKindT;
    dirty: boolean;
    val:   ValT;
  end;

var

  l1s:      array [CoreT] of L1StateT;
  l1_data:  array [CoreT] of ValT;
  l1_dirty: array [CoreT] of boolean;
  l1t_data:  array [CoreT] of ValT;
  l1t_dirty: array [CoreT] of boolean;
  l1t_pend:  array [CoreT] of AckT;

  l0_present: array [CoreT] of boolean;
  l0_dirty:   array [CoreT] of boolean;
  l0_data:    array [CoreT] of ValT;
  l0_recall:  array [CoreT] of boolean;
  l0q:      array [CoreT] of array [L0QIdx] of L0MsgT;
  l0q_cnt:  array [CoreT] of L0QCnt;

  spm_st:   array [CoreT] of SlotT;
  spm_data: array [CoreT] of ValT;

  pinned:      array [CoreT] of 0 .. MAXPIN;
  saw_migrate: array [CoreT] of boolean;
  no_free_way: boolean;

  cpu_op:   array [CoreT] of CpuOpT;
  retry_pending: array [CoreT] of boolean;
  cpu_busy: array [CoreT] of boolean;
  capture_pending: array[CoreT] of boolean;
  capture_value: array[CoreT] of ValT;

  l2s:       L2StateT;
  l2_sharer: array [CoreT] of boolean;
  l2_excl:   MaybeCore;
  l2_data:   ValT;
  l2_dirty:  boolean;
  l2t_data:  ValT;
  l2t_dirty: boolean;
  l2t_pend:  PendT;
  l2t_req:   CoreT;
  l2t_gets:  array [CoreT] of boolean;
  l2t_getx:  CoreT;

  dirs:     DirStateT;
  mem_data: ValT;
  memq:     MemOpT;
  memq_val: ValT;
  dir_inv:  0 .. 2;
  dma_used: 0 .. DMA_OPS;
  dma_req:  DmaReqT;
  dma_busy: boolean;
  pre_wb:   ValT;

  req:  array [ReqIdx] of ReqMsgT;
  fwd:  array [CoreT] of array [FwdIdx] of FwdMsgT;
  rspc: array [CoreT] of array [RspCIdx] of RspCMsgT;
  rsp2: array [Rsp2Idx] of Rsp2MsgT;
  unb:  array [UnbIdx] of UnbMsgT;
  dreq: array [DReqIdx] of DReqMsgT;
  d2d:  array [D2DIdx] of D2DMsgT;

  latest:  ValT;

  steps_used: 0 .. 63;
  stored_since_cpf: array [CoreT] of boolean;
  spm_expected:     array [CoreT] of ValT;

  cpf_sh0: array [CoreT] of boolean;
  cpf_ow0: array [CoreT] of boolean;

function inc_val(v: ValT): ValT;
begin
  if v = VMAX then
    return 0;
  else
    return v + 1;
  endif;
end;

function in_l0_cache(s: L1StateT): boolean;
begin

  return s = L1_S | s = L1_E | s = L1_M | s = L1_SM |
         s = L1_S_IL0 | s = L1_E_IL0 | s = L1_M_IL0 | s = L1_SM_IL0;
end;

function sharer_count(): PendT;
var n: PendT;
begin
  n := 0;
  for c: CoreT do
    if l2_sharer[c] then n := n + 1; endif;
  endfor;
  return n;
end;

function line_resident(c: CoreT): boolean;
begin
  return !(l1s[c] = L1_I | l1s[c] = L1_IX_D | l1s[c]=L1_IX_A);
end;

function line_stable(c: CoreT): boolean;
begin
  return l1s[c] = L1_S | l1s[c] = L1_SS | l1s[c] = L1_E |
         l1s[c] = L1_EE | l1s[c] = L1_M | l1s[c] = L1_MM;
end;

function spm_ways(c: CoreT): 0 .. MAXPIN+1;
begin
  if spm_st[c] = SP_INV then
    return pinned[c];
  else
    return pinned[c] + 1;
  endif;
end;

function free_ways(c: CoreT): 0 .. WAYS;
var used: 0 .. WAYS;
begin
  used := spm_ways(c);
  if line_resident(c) then used := used + 1; endif;
  return WAYS - used;
end;

function spmwb_inflight(): boolean;
begin
  return exists i: DReqIdx do dreq[i].valid & dreq[i].t = DQ_SPMWB endexists;
end;

procedure clear_req(i: ReqIdx);
begin
  req[i].valid := false; req[i].t := RQ_GETS; req[i].src := 0;
  req[i].val := 0; req[i].dirty := false;
end;

procedure push_req(t: ReqTypeT; src: CoreT; v: ValT; d: boolean);
var done: boolean;
begin
  done := false;
  for i: ReqIdx do
    if !done & !req[i].valid then
      req[i].valid := true; req[i].t := t; req[i].src := src;
      req[i].val := v; req[i].dirty := d;
      done := true;
    endif;
  endfor;
  if !done then error "request pool overflow (raise REQ_POOL)"; endif;
end;

procedure clear_fwd(c: CoreT; i: FwdIdx);
begin
  fwd[c][i].valid := false; fwd[c][i].t := FW_INV;
  fwd[c][i].req_is_l2 := false; fwd[c][i].req := 0;
end;

procedure push_fwd(c: CoreT; t: FwdTypeT; is_l2: boolean; r: CoreT);
var done: boolean;
begin
  done := false;
  for i: FwdIdx do
    if !done & !fwd[c][i].valid then
      fwd[c][i].valid := true; fwd[c][i].t := t;
      fwd[c][i].req_is_l2 := is_l2; fwd[c][i].req := r;
      done := true;
    endif;
  endfor;
  if !done then error "forward pool overflow (raise FWD_POOL)"; endif;
end;

procedure clear_rspc(c: CoreT; i: RspCIdx);
begin
  rspc[c][i].valid := false; rspc[c][i].t := RC_ACK; rspc[c][i].val := 0;
  rspc[c][i].dirty := false; rspc[c][i].ack := 0; rspc[c][i].from_l1 := false;
end;

procedure push_rspc(c: CoreT; t: RspCTypeT; v: ValT; d: boolean;
                    a: AckT; fl1: boolean);
var done: boolean;
begin
  done := false;
  for i: RspCIdx do
    if !done & !rspc[c][i].valid then
      rspc[c][i].valid := true; rspc[c][i].t := t; rspc[c][i].val := v;
      rspc[c][i].dirty := d; rspc[c][i].ack := a; rspc[c][i].from_l1 := fl1;
      done := true;
    endif;
  endfor;
  if !done then error "core response pool overflow (raise RSPC_POOL)"; endif;
end;

procedure clear_rsp2(i: Rsp2Idx);
begin
  rsp2[i].valid := false; rsp2[i].t := R2_ACK; rsp2[i].src := 0;
  rsp2[i].val := 0; rsp2[i].dirty := false;
end;

procedure push_rsp2(t: Rsp2TypeT; src: CoreT; v: ValT; d: boolean);
var done: boolean;
begin
  done := false;
  for i: Rsp2Idx do
    if !done & !rsp2[i].valid then
      rsp2[i].valid := true; rsp2[i].t := t; rsp2[i].src := src;
      rsp2[i].val := v; rsp2[i].dirty := d;
      done := true;
    endif;
  endfor;
  if !done then error "L2 response pool overflow (raise RSP2_POOL)"; endif;
end;

procedure clear_unb(i: UnbIdx);
begin
  unb[i].valid := false; unb[i].t := UB_UNB; unb[i].src := 0;
end;

procedure push_unb(t: UnbTypeT; src: CoreT);
var done: boolean;
begin
  done := false;
  for i: UnbIdx do
    if !done & !unb[i].valid then
      unb[i].valid := true; unb[i].t := t; unb[i].src := src;
      done := true;
    endif;
  endfor;
  if !done then error "unblock pool overflow (raise UNB_POOL)"; endif;
end;

procedure clear_dreq(i: DReqIdx);
begin
  dreq[i].valid := false; dreq[i].t := DQ_GETS; dreq[i].val := 0;
  dreq[i].inv_sent := false;
end;

procedure push_dreq(t: DReqTypeT; v: ValT);
var done: boolean;
begin
  done := false;
  for i: DReqIdx do
    if !done & !dreq[i].valid then
      dreq[i].valid := true; dreq[i].t := t; dreq[i].val := v;
      dreq[i].inv_sent := false;
      done := true;
    endif;
  endfor;
  if !done then error "dir request pool overflow (raise DREQ_POOL)"; endif;
end;

procedure clear_d2d(i: D2DIdx);
begin
  d2d[i].valid := false; d2d[i].t := DD_ACK; d2d[i].val := 0;
  d2d[i].dirty := false;
end;

procedure push_d2d(t: D2DTypeT; v: ValT; d: boolean);
var done: boolean;
begin
  done := false;
  for i: D2DIdx do
    if !done & !d2d[i].valid then
      d2d[i].valid := true; d2d[i].t := t; d2d[i].val := v; d2d[i].dirty := d;
      done := true;
    endif;
  endfor;
  if !done then error "L2->dir response pool overflow (raise D2D_POOL)"; endif;
end;

procedure l0q_push(c: CoreT; k: L0MsgKindT; d: boolean; v: ValT);
begin
  if l0q_cnt[c] = L0Q_DEPTH then
    error "L0->L1 queue overflow (raise L0Q_DEPTH)";
  endif;
  l0q[c][l0q_cnt[c]].kind := k;
  l0q[c][l0q_cnt[c]].dirty := d;
  l0q[c][l0q_cnt[c]].val := v;
  l0q_cnt[c] := l0q_cnt[c] + 1;
end;

procedure l0q_pop(c: CoreT);
begin
  for i: L0QIdx do
    if i + 1 < L0Q_DEPTH & i + 1 < l0q_cnt[c] then
      l0q[c][i] := l0q[c][i+1];
    endif;
  endfor;
  l0q_cnt[c] := l0q_cnt[c] - 1;
  l0q[c][l0q_cnt[c]].kind := L0M_IACK;
  l0q[c][l0q_cnt[c]].dirty := false;
  l0q[c][l0q_cnt[c]].val := 0;
end;

procedure ghost_store(v: ValT);
begin
  latest := v;
  for o: CoreT do
    stored_since_cpf[o] := true;
  endfor;
end;

procedure spm_install(c: CoreT; v: ValT);
begin
  spm_st[c] := SP_X;
  spm_data[c] := v;
  spm_expected[c] := v;

  if !stored_since_cpf[c] & true then
    assert v = latest "SPMCP snapshot missed the latest coherent value";
  endif;
end;

procedure assert_silent(c: CoreT);
begin
 assert cpu_op[c]=OP_CPF "silent completion without original copy operation";
end;

procedure cpu_done(c: CoreT);
begin
  cpu_op[c] := OP_NONE;
  cpu_busy[c] := false;
end;

procedure apply_store(c: CoreT);
begin
  l0_present[c] := true;
  l0_dirty[c] := true;
  l0_data[c] := inc_val(latest);
  ghost_store(l0_data[c]);
  cpu_done(c);
end;

procedure send_recall(c: CoreT);
begin
  l0_recall[c] := true;
end;

procedure l2_dealloc();
begin
  for c: CoreT do l2_sharer[c] := false; endfor;
  l2_excl := NCORE;
  l2_data := 0;
  l2_dirty := false;
end;

procedure l2_clear_tbe();
begin
  l2t_data := 0; l2t_dirty := false; l2t_pend := 0; l2t_req := 0;
  for c: CoreT do l2t_gets[c] := false; endfor;
  l2t_getx := 0;
end;

function line_quiet_for_wb(): boolean;
var q: boolean;
begin
  q := true;
  for c: CoreT do
    if l1s[c] != L1_I | l0_present[c] | l0_recall[c] | l0q_cnt[c] != 0 then
      q := false;
    endif;
    for i: FwdIdx do
      if fwd[c][i].valid then q := false; endif;
    endfor;
    for i: RspCIdx do
      if rspc[c][i].valid then q := false; endif;
    endfor;
  endfor;
  for i: ReqIdx do

    if req[i].valid & req[i].t != RQ_SPMWB then q := false; endif;
  endfor;
  for i: Rsp2Idx do
    if rsp2[i].valid then q := false; endif;
  endfor;
  for i: UnbIdx do
    if unb[i].valid then q := false; endif;
  endfor;
  for i: DReqIdx do
    if dreq[i].valid then q := false; endif;
  endfor;
  for i: D2DIdx do
    if d2d[i].valid then q := false; endif;
  endfor;
  if memq != MQ_NONE then q := false; endif;
  return q;
end;

procedure relinquish_source(c:CoreT);
begin
 if RETAIN_SOURCE=1 then
  spm_install(c,l1_data[c]);cpu_done(c);
 else
 cpu_busy[c]:=true;
 if l1s[c]=L1_SS then
  l1s[c]:=L1_SX_A;push_req(RQ_PUTS,c,l1_data[c],false);
 elsif l1s[c]=L1_EE then
  l1s[c]:=L1_EX_A;push_req(RQ_PUTE,c,l1_data[c],false);
 else
  assert l1s[c]=L1_MM;
  l1s[c]:=L1_MX_A;push_req(RQ_PUTM,c,l1_data[c],true);
 endif;
 endif;
end;

procedure commit_spm_writeback();
begin
 assert l2t_pend = 0 "writeback committed before invalidation completion";
 if DMA_OPS > 0 then pre_wb := latest; endif;
 for o: CoreT do l2_sharer[o] := false; endfor;
 l2_excl := NCORE;
 l2_data := l2t_data; l2_dirty := true;
 ghost_store(l2t_data);
 push_dreq(DQ_SPMWB,l2t_data);
 l2s := L2_SPM_WB;
end;

startstate "cold system"
begin
  for c: CoreT do
    l1s[c] := L1_I; l1_data[c] := 0; l1_dirty[c] := false;
    l1t_data[c] := 0; l1t_dirty[c] := false; l1t_pend[c] := 0;
    l0_present[c] := false; l0_dirty[c] := false; l0_data[c] := 0;
    l0_recall[c] := false;
    for i: L0QIdx do
      l0q[c][i].kind := L0M_IACK; l0q[c][i].dirty := false; l0q[c][i].val := 0;
    endfor;
    l0q_cnt[c] := 0;
    spm_st[c] := SP_INV; spm_data[c] := 0;
    pinned[c] := 0; saw_migrate[c] := false;
    retry_pending[c] := false;
    cpu_op[c] := OP_NONE; cpu_busy[c] := false;capture_pending[c]:=false;capture_value[c]:=0;
    stored_since_cpf[c] := false; spm_expected[c] := 0;
    cpf_sh0[c] := false; cpf_ow0[c] := false;
    for i: FwdIdx do clear_fwd(c, i); endfor;
    for i: RspCIdx do clear_rspc(c, i); endfor;
  endfor;
  l2s := L2_NP; l2_dealloc(); l2_clear_tbe();
  dirs := D_I; mem_data := 0; memq := MQ_NONE; memq_val := 0; dir_inv := 0;
  dma_used := 0; dma_req := DMA_NONE; dma_busy := false; pre_wb := 0;
  for i: ReqIdx do clear_req(i); endfor;
  for i: Rsp2Idx do clear_rsp2(i); endfor;
  for i: UnbIdx do clear_unb(i); endfor;
  for i: DReqIdx do clear_dreq(i); endfor;
  for i: D2DIdx do clear_d2d(i); endfor;
  latest := 0;  steps_used := 0; no_free_way := false;
endstartstate;

ruleset c: CoreT do

  rule "cpu issue Load"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_LD;
  endrule;

  rule "cpu issue Store"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_ST;
  endrule;

  rule "cpu issue SPMCP_fetch"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE &
    (spm_st[c] != SP_INV |
     (spm_ways(c) + 1 <= WAYS - MIN_FREE & free_ways(c) >= 1))
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_CPF;
    stored_since_cpf[c] := false;
  endrule;

  rule "cpu issue SPMCP_install"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE & spm_st[c] = SP_INV
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_INST;
  endrule;

  rule "cpu issue SPMLD"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_SLD;
  endrule;

  rule "cpu issue SPMST"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_SST;
  endrule;

  rule "cpu issue SPMWB_store"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_SWB;
  endrule;

  rule "cpu issue SPM_release"
    (STEPS = 0 | steps_used < STEPS) &
    cpu_op[c] = OP_NONE
  ==>
  begin
    steps_used := steps_used + 1;
    cpu_op[c] := OP_REL;
  endrule;

endruleset;

ruleset c: CoreT do

  rule "L0 load hit"
    cpu_op[c] = OP_LD & !cpu_busy[c] & l0_present[c] & in_l0_cache(l1s[c])
  ==>
  begin
    if true then
      assert l0_data[c] = latest "L0 load hit returned a stale value";
    endif;
    cpu_done(c);
  endrule;

  rule "L0 store hit"
    cpu_op[c] = OP_ST & !cpu_busy[c] & l0_present[c] &
    (l1s[c] = L1_E | l1s[c] = L1_M)
  ==>
  begin
    apply_store(c);
  endrule;

  rule "L1 Load I -> IS (GETS)"
    cpu_op[c] = OP_LD & !cpu_busy[c] & !l0_present[c] & l1s[c] = L1_I &
    l0q_cnt[c] = 0 & free_ways(c) >= 1
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_IS;
    l1t_pend[c] := 0;
    push_req(RQ_GETS, c, 0, false);
  endrule;

  rule "L1 Load hit S/SS"
    cpu_op[c] = OP_LD & !cpu_busy[c] & !l0_present[c] &
    (l1s[c] = L1_S | l1s[c] = L1_SS) & l0q_cnt[c] = 0
  ==>
  begin

    l1s[c] := L1_S;
    l0_present[c] := true; l0_dirty[c] := false; l0_data[c] := l1_data[c];
    if true then
      assert l1_data[c] = latest "shared load hit returned a stale value";
    endif;
    cpu_done(c);
  endrule;

  rule "L1 Load hit E/EE"
    cpu_op[c] = OP_LD & !cpu_busy[c] & !l0_present[c] &
    (l1s[c] = L1_E | l1s[c] = L1_EE) & l0q_cnt[c] = 0
  ==>
  begin
    l1s[c] := L1_E;
    l0_present[c] := true; l0_dirty[c] := false; l0_data[c] := l1_data[c];
    if true then
      assert l1_data[c] = latest "exclusive load hit returned a stale value";
    endif;
    cpu_done(c);
  endrule;

  rule "L1 Load hit M/MM"
    cpu_op[c] = OP_LD & !cpu_busy[c] & !l0_present[c] &
    (l1s[c] = L1_M | l1s[c] = L1_MM) & l0q_cnt[c] = 0
  ==>
  begin
    l1s[c] := L1_M;
    l0_present[c] := true; l0_dirty[c] := false; l0_data[c] := l1_data[c];
    if true then
      assert l1_data[c] = latest "modified load hit returned a stale value";
    endif;
    cpu_done(c);
  endrule;

  rule "L1 Store I -> IM (GETX)"
    cpu_op[c] = OP_ST & !cpu_busy[c] & !l0_present[c] & l1s[c] = L1_I &
    l0q_cnt[c] = 0 & free_ways(c) >= 1
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_IM;
    l1t_pend[c] := 0;
    push_req(RQ_GETX, c, 0, false);
  endrule;

  rule "L1 Store S/SS -> SM (UPGRADE)"
    cpu_op[c] = OP_ST & !cpu_busy[c] &
    (l1s[c] = L1_S | l1s[c] = L1_SS) & l0q_cnt[c] = 0
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_SM;
    l1t_pend[c] := 0;
    push_req(RQ_UPG, c, 0, false);
  endrule;

  rule "L1 Store hit E/M/EE/MM -> M"
    cpu_op[c] = OP_ST & !cpu_busy[c] & !l0_present[c] &
    (l1s[c] = L1_E | l1s[c] = L1_M | l1s[c] = L1_EE | l1s[c] = L1_MM) & l0q_cnt[c] = 0
  ==>
  begin
    l1s[c] := L1_M;
    apply_store(c);
  endrule;

  rule "L1 SPMCP_fetch I -> IX_D (GETS_SILENT)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] & l1s[c] = L1_I & l0q_cnt[c] = 0
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_IX_D;
    cpf_sh0[c] := l2_sharer[c];
    cpf_ow0[c] := (l2_excl = c);
    push_req(RQ_SILENT, c, 0, false);
  endrule;

  rule "L0 captures retained coherent snapshot"
    RETAIN_SOURCE=1 & cpu_op[c]=OP_CPF & !cpu_busy[c] &
    l0_present[c] & (l0q_cnt[c]=0 | FINITE_RETURN_LANE=1) &
    (l1s[c]=L1_S | l1s[c]=L1_E | l1s[c]=L1_M)
  ==>
  begin
    cpu_busy[c]:=true;capture_pending[c]:=true;
    if M_CAPTURE_FROM_L1=1 then capture_value[c]:=l1_data[c]; else capture_value[c]:=l0_data[c]; endif;
  endrule;

  rule "L1 installs retained L0 snapshot"
    capture_pending[c] &
    (l1s[c]=L1_I | l1s[c]=L1_S | l1s[c]=L1_E | l1s[c]=L1_M |
     l1s[c]=L1_SS | l1s[c]=L1_EE | l1s[c]=L1_MM)
  ==>
  begin
    spm_install(c,capture_value[c]);capture_pending[c]:=false;
    capture_value[c]:=0;cpu_done(c);
  endrule;

  rule "L1 SPMCP_fetch S -> SX_L0 (recall L0)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] & l1s[c] = L1_S &
    (l0q_cnt[c] = 0 | FINITE_RETURN_LANE=1) &
    (RETAIN_SOURCE=0 | !l0_present[c])
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_SX_L0;
    send_recall(c);
  endrule;

  rule "L1 SPMCP_fetch E -> EX_L0 (recall L0)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] & l1s[c] = L1_E &
    (l0q_cnt[c] = 0 | FINITE_RETURN_LANE=1) &
    (RETAIN_SOURCE=0 | !l0_present[c])
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_EX_L0;
    send_recall(c);
  endrule;

  rule "L1 SPMCP_fetch M -> MX_L0 (recall L0)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] & l1s[c] = L1_M &
    (l0q_cnt[c] = 0 | FINITE_RETURN_LANE=1) &
    (RETAIN_SOURCE=0 | !l0_present[c])
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_MX_L0;
    send_recall(c);
  endrule;

  rule "L1 SPMCP_fetch hit SS/EE/MM (install from cache)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] &
    (l1s[c] = L1_SS | l1s[c] = L1_EE | l1s[c] = L1_MM) & l0q_cnt[c] = 0
  ==>
  begin
    relinquish_source(c);
  endrule;

  rule "L1 SPMCP_install (free way, no displacement)"
    cpu_op[c] = OP_INST & !cpu_busy[c] & spm_st[c] = SP_INV & l0q_cnt[c] = 0 &
    spm_ways(c) + 1 <= WAYS - MIN_FREE & free_ways(c) >= 1
  ==>
  begin
    spm_st[c] := SP_X;
    spm_data[c] := 0;
    spm_expected[c] := 0;
    cpu_done(c);
  endrule;

  rule "L1 SPMCP_install (displaces + migrates coherent line)"
    cpu_op[c] = OP_INST & !cpu_busy[c] & spm_st[c] = SP_INV & l0q_cnt[c] = 0 &
    line_resident(c) &
    spm_ways(c) + 1 <= WAYS - MIN_FREE &
    (INSTALL_STABLE_ONLY = 0 | line_stable(c))
  ==>
  begin
    if free_ways(c) >= 1 then

      saw_migrate[c] := true;
      spm_st[c] := SP_X;
      spm_data[c] := 0;
      spm_expected[c] := 0;
      cpu_done(c);
    else

      no_free_way := true;
      cpu_done(c);
    endif;
  endrule;

  rule "env pin extra SPM slot"
    pinned[c] < MAXPIN & spm_ways(c) + 1 <= WAYS - MIN_FREE & free_ways(c) >= 1 &

    !(cpu_op[c] = OP_CPF | cpu_op[c] = OP_INST)
  ==>
  begin
    pinned[c] := pinned[c] + 1;
  endrule;

  rule "env release pinned SPM slot"
    pinned[c] > 0
  ==>
  begin
    pinned[c] := pinned[c] - 1;
  endrule;

  rule "L1 SPMLD"
    cpu_op[c] = OP_SLD & !cpu_busy[c] & spm_st[c] != SP_XWB
  ==>
  begin
    if spm_st[c] = SP_X then
      assert spm_data[c] = spm_expected[c]
        "SPM load observed a corrupted slot value";
    endif;

    cpu_done(c);
  endrule;

  rule "L1 SPMST"
    cpu_op[c] = OP_SST & !cpu_busy[c] & spm_st[c] != SP_XWB
  ==>
  begin
    if spm_st[c] = SP_X then
      spm_data[c] := inc_val(spm_data[c]);
      spm_expected[c] := spm_data[c];
    endif;

    cpu_done(c);
  endrule;

  rule "L1 SPMWB_store X -> XWB (SPMWB_REQ)"
    cpu_op[c] = OP_SWB & !cpu_busy[c] & spm_st[c] = SP_X & l0q_cnt[c] = 0
  ==>
  begin
    cpu_busy[c] := true;
    spm_st[c] := SP_XWB;
    push_req(RQ_SPMWB, c, spm_data[c], true);
  endrule;

  rule "L1 SPMWB_store on non-X slot acks"
    cpu_op[c] = OP_SWB & !cpu_busy[c] & spm_st[c] = SP_INV & l0q_cnt[c] = 0
  ==>
  begin
    cpu_done(c);
  endrule;

  rule "L1 SPM_release"
    cpu_op[c] = OP_REL & !cpu_busy[c] & spm_st[c] != SP_XWB & l0q_cnt[c] = 0
  ==>
  begin
    if spm_st[c] = SP_X then
      spm_st[c] := SP_INV;
      spm_data[c] := 0;
      spm_expected[c] := 0;
    endif;
    cpu_done(c);
  endrule;

endruleset;

ruleset c: CoreT do

  rule "L0 answers recall"
    l0_recall[c]
  ==>
  begin
    l0_recall[c] := false;
    if l0_present[c] & l0_dirty[c] then
      l0q_push(c, L0M_IDATA, true, l0_data[c]);
    else
      l0q_push(c, L0M_IACK, false, 0);
    endif;
    l0_present[c] := false;
    l0_dirty[c] := false;
    l0_data[c] := 0;
  endrule;

  rule "L0 silent clean S drop"
    l0_present[c] & !l0_dirty[c] &
    (l1s[c] = L1_S | l1s[c] = L1_S_IL0 | l1s[c] = L1_SX_L0)
  ==>
  begin
    l0_present[c] := false;
    l0_data[c] := 0;
  endrule;

  rule "L0 evicts E/M copy (WriteBack)"
    l0_present[c] &
    (l1s[c] = L1_E | l1s[c] = L1_M |
     l1s[c] = L1_E_IL0 | l1s[c] = L1_M_IL0 |
     l1s[c] = L1_EX_L0 | l1s[c] = L1_MX_L0)
  ==>
  begin
    l0q_push(c, L0M_WB, l0_dirty[c], l0_data[c]);
    l0_present[c] := false;
    l0_dirty[c] := false;
    l0_data[c] := 0;
  endrule;

endruleset;

ruleset c: CoreT do

  rule "L1 handles L0 queue head"
    l0q_cnt[c] > 0
  ==>
  var k: L0MsgKindT; d: boolean; v: ValT;
  begin
    k := l0q[c][0].kind; d := l0q[c][0].dirty; v := l0q[c][0].val;
    l0q_pop(c);

    if k = L0M_WB then

      if l1s[c] = L1_M | l1s[c] = L1_E then
        if d then l1_data[c] := v; l1_dirty[c] := true; endif;
        l1s[c] := L1_MM;
      elsif l1s[c] = L1_M_IL0 | l1s[c] = L1_E_IL0 then
        if d then l1_data[c] := v; l1_dirty[c] := true; endif;
        l1s[c] := L1_MM_IL0;
      elsif IMPLEMENTED_TRANSITIONS = 1 &
            (l1s[c] = L1_MX_L0 | l1s[c] = L1_EX_L0) then

        if d then l1_data[c] := v; l1_dirty[c] := true; endif;
      else

        error "L1: WriteBack from L0 in a state with no SLICC transition";
      endif;

    elsif k = L0M_IACK then

      if l1s[c] = L1_S_IL0 then l1s[c] := L1_SS;
      elsif l1s[c] = L1_E_IL0 then l1s[c] := L1_EE;
      elsif l1s[c] = L1_M_IL0 then l1s[c] := L1_MM;
      elsif l1s[c] = L1_MM_IL0 then l1s[c] := L1_MM;
      elsif l1s[c] = L1_SM_IL0 then l1s[c] := L1_IM;
      elsif l1s[c] = L1_SX_L0 then
        l1s[c] := L1_SS; relinquish_source(c);
      elsif l1s[c] = L1_EX_L0 then

        if IMPLEMENTED_TRANSITIONS = 1 & l1_dirty[c] then
          l1s[c] := L1_MM;
        else
          l1s[c] := L1_EE;
        endif;
        relinquish_source(c);
      elsif l1s[c] = L1_MX_L0 then
        l1s[c] := L1_MM; relinquish_source(c);
      else
        error "L1: L0_Ack in a state with no SLICC transition";
      endif;

    else

      if l1s[c] = L1_M_IL0 | l1s[c] = L1_E_IL0 then
        l1_data[c] := v; l1_dirty[c] := true;
        l1s[c] := L1_MM;
      elsif l1s[c] = L1_SX_L0 then
        l1_data[c] := v; l1_dirty[c] := true;
        l1s[c] := L1_SS; relinquish_source(c);
      elsif l1s[c] = L1_EX_L0 then
        l1_data[c] := v; l1_dirty[c] := true;

        if IMPLEMENTED_TRANSITIONS = 1 then
          l1s[c] := L1_MM;
        else
          l1s[c] := L1_EE;
        endif;
        relinquish_source(c);
      elsif l1s[c] = L1_MX_L0 then
        l1_data[c] := v; l1_dirty[c] := true;
        l1s[c] := L1_MM; relinquish_source(c);
      else
        error "L1: L0_DataAck in a state with no SLICC transition";
      endif;
    endif;
  endrule;

endruleset;

ruleset c: CoreT do

  rule "L1 victimize S/E/M (recall L0 first)"
    (l1s[c] = L1_S | l1s[c] = L1_E | l1s[c] = L1_M)
  ==>
  begin
    if l1s[c] = L1_S then l1s[c] := L1_S_IL0;
    elsif l1s[c] = L1_E then l1s[c] := L1_E_IL0;
    else l1s[c] := L1_M_IL0;
    endif;
    send_recall(c);
  endrule;

  rule "L1 replacement SS -> I (silent)"
    l1s[c] = L1_SS
  ==>
  begin
    l1s[c] := L1_I;
    l1_data[c] := 0; l1_dirty[c] := false;
  endrule;

  rule "L1 replacement EE/MM -> M_I (PUTX)"
    (l1s[c] = L1_EE | l1s[c] = L1_MM)
  ==>
  begin
    l1t_data[c] := l1_data[c];
    l1t_dirty[c] := l1_dirty[c];
    push_req(RQ_PUTX, c, l1_data[c], l1_dirty[c]);
    l1s[c] := L1_M_I;
    l1_data[c] := 0; l1_dirty[c] := false;
  endrule;

endruleset;

ruleset c: CoreT; i: FwdIdx do

  rule "L1 fwd triggers L0_Invalidate_Else"
    fwd[c][i].valid &
    (l1s[c] = L1_S | l1s[c] = L1_E | l1s[c] = L1_M | l1s[c] = L1_SM)
  ==>
  begin
    if l1s[c] = L1_S then l1s[c] := L1_S_IL0;
    elsif l1s[c] = L1_E then l1s[c] := L1_E_IL0;
    elsif l1s[c] = L1_M then l1s[c] := L1_M_IL0;
    else l1s[c] := L1_SM_IL0;
    endif;
    send_recall(c);
  endrule;

  rule "forward while local SPM PUT is pending"
    fwd[c][i].valid &
    (l1s[c]=L1_SX_A | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | l1s[c]=L1_IX_A) &
    !(l1s[c]=L1_SX_A & fwd[c][i].t=FW_GETS) &
    !(l1s[c]=L1_IX_A & (fwd[c][i].t=FW_GETS | fwd[c][i].t=FW_GETX))
  ==>
  var rq:CoreT;rl2:boolean;t:FwdTypeT;
  begin
    rq:=fwd[c][i].req;rl2:=fwd[c][i].req_is_l2;t:=fwd[c][i].t;
    clear_fwd(c,i);
    if t=FW_SILENT then
      if l1s[c]=L1_IX_A then push_rspc(rq,RC_RETRY,0,false,0,true);
      else push_rspc(rq,RC_DATA,l1_data[c],false,0,true);endif;
    elsif t=FW_GETS then
      push_rspc(rq,RC_DATA,l1_data[c],l1_dirty[c],0,true);
      push_rsp2(R2_WBDATA,c,l1_data[c],l1_dirty[c]);l1s[c]:=L1_SX_A;
    else
      if l1s[c]!=L1_IX_A then l1t_data[c]:=l1_data[c];endif;
      if t=FW_GETX then push_rspc(rq,RC_DATA,l1t_data[c],l1_dirty[c],0,true);
      elsif l1s[c]=L1_EX_A | l1s[c]=L1_MX_A then push_rsp2(R2_WBDATA,c,l1t_data[c],l1_dirty[c]);
      elsif rl2 then push_rsp2(R2_ACK,c,0,false);
      else push_rspc(rq,RC_ACK,0,false,1,true);endif;
      l1s[c]:=L1_IX_A;l1_data[c]:=0;l1_dirty[c]:=false;
    endif;
  endrule;

  rule "L1 handles Inv"
    !(l1s[c]=L1_SX_A | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | l1s[c]=L1_IX_A) &
    fwd[c][i].valid & fwd[c][i].t = FW_INV &
    !in_l0_cache(l1s[c]) &

    !(l1s[c] = L1_MM_IL0 | l1s[c] = L1_SX_L0 | l1s[c] = L1_MX_L0 |
      l1s[c] = L1_EX_L0) &

    (IMPLEMENTED_TRANSITIONS = 1 | l1s[c] != L1_IX_D)
  ==>
  var rq: CoreT; rl2: boolean;
  begin
    rq := fwd[c][i].req; rl2 := fwd[c][i].req_is_l2;
    clear_fwd(c, i);
    if l1s[c] = L1_I | l1s[c] = L1_IX_D then
      if rl2 then push_rsp2(R2_ACK, c, 0, false);
      else push_rspc(rq, RC_ACK, 0, false, 1, true); endif;
    elsif l1s[c] = L1_SS then
      if rl2 then push_rsp2(R2_ACK, c, 0, false);
      else push_rspc(rq, RC_ACK, 0, false, 1, true); endif;
      if M_INV_KEEPS_COPY=0 then l1s[c] := L1_I; l1_data[c] := 0; l1_dirty[c] := false; endif;
    elsif l1s[c] = L1_EE then
      if rl2 then push_rsp2(R2_ACK, c, 0, false);
      else push_rspc(rq, RC_ACK, 0, false, 1, true); endif;
      l1s[c] := L1_I; l1_data[c] := 0; l1_dirty[c] := false;
    elsif l1s[c] = L1_MM then

      push_rsp2(R2_WBDATA, c, l1_data[c], l1_dirty[c]);
      l1s[c] := L1_I; l1_data[c] := 0; l1_dirty[c] := false;
    elsif l1s[c] = L1_IS | l1s[c] = L1_IS_I then
      if rl2 then push_rsp2(R2_ACK, c, 0, false);
      else push_rspc(rq, RC_ACK, 0, false, 1, true); endif;
      l1s[c] := L1_IS_I;
    elsif l1s[c] = L1_IM then
      if rl2 then push_rsp2(R2_ACK, c, 0, false);
      else push_rspc(rq, RC_ACK, 0, false, 1, true); endif;
    elsif l1s[c] = L1_M_I then

      push_rsp2(R2_WBDATA, c, l1t_data[c], l1t_dirty[c]);
      l1s[c] := L1_SINK;
    elsif l1s[c] = L1_SINK then
      if rl2 then push_rsp2(R2_ACK, c, 0, false);
      else push_rspc(rq, RC_ACK, 0, false, 1, true); endif;
    else
      error "L1: Inv in a state with no SLICC transition";
    endif;
  endrule;

  rule "L1 handles Fwd_GETX"
    !(l1s[c]=L1_SX_A | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | l1s[c]=L1_IX_A) &
    fwd[c][i].valid & fwd[c][i].t = FW_GETX &
    !in_l0_cache(l1s[c]) &
    !(l1s[c] = L1_MM_IL0 | l1s[c] = L1_SX_L0 | l1s[c] = L1_MX_L0 |
      l1s[c] = L1_EX_L0 | l1s[c] = L1_IX_D)
  ==>
  var rq: CoreT;
  begin
    rq := fwd[c][i].req;
    clear_fwd(c, i);
    if l1s[c] = L1_EE | l1s[c] = L1_MM then
      push_rspc(rq, RC_DATA, l1_data[c], l1_dirty[c], 0, true);
      l1s[c] := L1_I; l1_data[c] := 0; l1_dirty[c] := false;
    elsif l1s[c] = L1_M_I then
      push_rspc(rq, RC_DATA, l1t_data[c], l1t_dirty[c], 0, true);
      l1s[c] := L1_SINK;
    else
      error "L1: Fwd_GETX in a state with no SLICC transition";
    endif;
  endrule;

  rule "L1 handles Fwd_GETS"
    !(l1s[c]=L1_SX_A | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | l1s[c]=L1_IX_A) &
    fwd[c][i].valid & fwd[c][i].t = FW_GETS &
    !in_l0_cache(l1s[c]) &
    !(l1s[c] = L1_MM_IL0 | l1s[c] = L1_SX_L0 | l1s[c] = L1_MX_L0 |
      l1s[c] = L1_EX_L0 | l1s[c] = L1_IX_D)
  ==>
  var rq: CoreT;
  begin
    rq := fwd[c][i].req;
    clear_fwd(c, i);
    if l1s[c] = L1_EE | l1s[c] = L1_MM then
      push_rspc(rq, RC_DATA, l1_data[c], l1_dirty[c], 0, true);
      push_rsp2(R2_WBDATA, c, l1_data[c], l1_dirty[c]);
      l1s[c] := L1_SS;
    elsif l1s[c] = L1_M_I then
      push_rspc(rq, RC_DATA, l1t_data[c], l1t_dirty[c], 0, true);
      push_rsp2(R2_WBDATA, c, l1t_data[c], l1t_dirty[c]);
      l1s[c] := L1_SINK;
    else
      error "L1: Fwd_GETS in a state with no SLICC transition";
    endif;
  endrule;

  rule "L1 handles Fwd_GETS_Silent"
    !(l1s[c]=L1_SX_A | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | l1s[c]=L1_IX_A) &
    fwd[c][i].valid & fwd[c][i].t = FW_SILENT &
    !in_l0_cache(l1s[c]) &

    !(l1s[c] = L1_SX_L0 | l1s[c] = L1_MX_L0 | l1s[c] = L1_EX_L0 |
      l1s[c] = L1_IX_D | l1s[c] = L1_SINK) &

    !(l1s[c] = L1_MM_IL0)
  ==>
  var rq: CoreT;
  begin
    rq := fwd[c][i].req;
    clear_fwd(c, i);
    if l1s[c] = L1_MM | l1s[c] = L1_EE then

      push_rspc(rq, RC_DATA, l1_data[c], false, 0, true);
      if FNF_SILENT = 0 then push_unb(UB_UNB, c); endif;
    elsif FNF_SILENT = 1 &
          (l1s[c] = L1_S | l1s[c] = L1_SS | l1s[c] = L1_SM) then

      push_rspc(rq, RC_DATA, l1_data[c], false, 0, true);
    elsif FNF_SILENT = 1 &
          (l1s[c] = L1_I | l1s[c] = L1_IS | l1s[c] = L1_IM |
           l1s[c] = L1_IS_I) then

      push_rspc(rq, RC_RETRY, 0, false, 0, true);
    elsif l1s[c] = L1_M_I then

      push_rspc(rq, RC_DATA, l1t_data[c], false, 0, true);
      if FNF_SILENT = 1 then

        l1s[c] := L1_M_I;
      elsif IMPLEMENTED_TRANSITIONS = 1 then
        push_rsp2(R2_WBDATA, c, l1t_data[c], l1t_dirty[c]);
        l1s[c] := L1_SINK;
      else
        push_unb(UB_UNB, c);
        l1s[c] := L1_SINK;
      endif;
    else

      error "L1: Fwd_GETS_Silent in a state with no SLICC transition";
    endif;
  endrule;

endruleset;

ruleset c: CoreT do
 rule "reissue silent fetch from existing TBE"
  retry_pending[c] & exists i: ReqIdx do !req[i].valid endexists
 ==>
 begin
  assert l1s[c] = L1_IX_D "retry lost its original TBE";
  push_req(RQ_SILENT,c,0,false);
  retry_pending[c] := false;
 endrule;
endruleset;

ruleset c: CoreT; i: RspCIdx do

 rule "SPM PUT acknowledgment installs saved snapshot"
  rspc[c][i].valid & rspc[c][i].t=RC_PUTACK
 ==>
 begin
  assert l1s[c]=L1_SX_A | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | l1s[c]=L1_IX_A;
  clear_rspc(c,i);
  if l1s[c]=L1_IX_A then spm_install(c,l1t_data[c]);
  else spm_install(c,l1_data[c]);endif;
  l1s[c]:=L1_I;l1_data[c]:=0;l1_dirty[c]:=false;cpu_done(c);
 endrule;

 rule "consume silent NACK into existing TBE"
  rspc[c][i].valid & rspc[c][i].t = RC_RETRY
 ==>
 begin
  assert l1s[c] = L1_IX_D & !retry_pending[c] "invalid or duplicate retry";
  clear_rspc(c,i); if M_RETRY_DROPPED=0 then retry_pending[c] := true; endif;
 endrule;

  rule "L1 handles DATA"
    rspc[c][i].valid & rspc[c][i].t = RC_DATA
  ==>
  var v: ValT; d: boolean; a: AckT; fl1: boolean;
  begin
    v := rspc[c][i].val; d := rspc[c][i].dirty;
    a := rspc[c][i].ack; fl1 := rspc[c][i].from_l1;
    clear_rspc(c, i);

    if l1s[c] = L1_IS then
      l1_data[c] := v; l1_dirty[c] := d;
      if fl1 then push_unb(UB_UNB, c); endif;
      if !fl1 & a != 0 then
        error "L1: IS data with outstanding acks is not a SLICC path";
      endif;
      l1s[c] := L1_S;
      l0_present[c] := true; l0_dirty[c] := false; l0_data[c] := v;
      if true then
        assert v = latest "load miss returned a stale value";
      endif;
      cpu_done(c);
    elsif l1s[c] = L1_IS_I then

      if fl1 then push_unb(UB_UNB, c); endif;
      l1s[c] := L1_I;
      l1_data[c] := 0; l1_dirty[c] := false;
      cpu_done(c);
    elsif l1s[c] = L1_IM then
      if l1t_pend[c] - a = 0 then

        l1_data[c] := v; l1_dirty[c] := d;
        push_unb(UB_XUNB, c);
        l1s[c] := L1_M;
        apply_store(c);
      else

        l1_data[c] := v; l1_dirty[c] := d;
        l1t_pend[c] := l1t_pend[c] - a;
        l1s[c] := L1_SM;
      endif;
    elsif l1s[c] = L1_IX_D then

      l1s[c] := L1_I;
      spm_install(c, v);
      assert_silent(c);
      cpu_done(c);
    else
      error "L1: DATA in a state with no SLICC transition";
    endif;
  endrule;

  rule "L1 handles DATA_EXCLUSIVE"
    rspc[c][i].valid & rspc[c][i].t = RC_DATAX
  ==>
  var v: ValT;
  begin
    v := rspc[c][i].val;
    clear_rspc(c, i);
    if l1s[c] = L1_IS | l1s[c] = L1_IS_I then
      l1_data[c] := v; l1_dirty[c] := false;
      push_unb(UB_XUNB, c);
      l1s[c] := L1_E;
      l0_present[c] := true; l0_dirty[c] := false; l0_data[c] := v;
      if true then
        assert v = latest "exclusive load miss returned a stale value";
      endif;
      cpu_done(c);
    elsif l1s[c] = L1_IX_D then
      l1s[c] := L1_I;
      spm_install(c, v);
      assert_silent(c);
      cpu_done(c);
    else
      error "L1: DATA_EXCLUSIVE in a state with no SLICC transition";
    endif;
  endrule;

  rule "L1 handles ACK"
    rspc[c][i].valid & rspc[c][i].t = RC_ACK
  ==>
  var a: AckT;
  begin
    a := rspc[c][i].ack;
    clear_rspc(c, i);
    if l1s[c] = L1_SM then
      if l1t_pend[c] - a = 0 then

        push_unb(UB_XUNB, c);
        l1s[c] := L1_M;
        apply_store(c);
      else
        l1t_pend[c] := l1t_pend[c] - a;
      endif;
    elsif l1s[c] = L1_IM then
      if l1t_pend[c] - a = 0 then
        error "L1: IM Ack_all has no SLICC transition";
      else
        l1t_pend[c] := l1t_pend[c] - a;
      endif;
    else
      error "L1: ACK in a state with no SLICC transition";
    endif;
  endrule;

  rule "L1 handles WB_Ack"
    rspc[c][i].valid & rspc[c][i].t = RC_WBACK
  ==>
  begin
    clear_rspc(c, i);
    if l1s[c] = L1_M_I then
      l1s[c] := L1_I;
      l1t_data[c] := 0; l1t_dirty[c] := false;
    elsif l1s[c] = L1_SINK then
      l1s[c] := L1_I;
      l1t_data[c] := 0; l1t_dirty[c] := false;
    else
      error "L1: WB_Ack in a state with no SLICC transition";
    endif;
  endrule;

  rule "L1 handles SPMWB_Ack"
    rspc[c][i].valid & rspc[c][i].t = RC_SPMWBACK
  ==>
  begin
    clear_rspc(c, i);
    if spm_st[c] = SP_XWB then
      spm_st[c] := SP_X;
      cpu_done(c);
    else
      error "L1: SPMWB_Ack with slot not in XWB has no SLICC transition";
    endif;
  endrule;

endruleset;

ruleset i: ReqIdx do

 rule "L2 accepts local-source SPM PUT"
  req[i].valid & (req[i].t=RQ_PUTS | req[i].t=RQ_PUTE | req[i].t=RQ_PUTM) &
  (l2s=L2_NP | l2s=L2_SS | l2s=L2_M | l2s=L2_MT)
 ==>
 var c:CoreT;
 begin
  c:=req[i].src;
  if l2_sharer[c] then
    if req[i].t=RQ_PUTM then l2_data:=req[i].val;l2_dirty:=true;endif;
    l2_sharer[c]:=false;
    if l2s=L2_MT then l2_excl:=NCORE;l2s:=L2_M;endif;
  endif;
  push_rspc(c,RC_PUTACK,0,false,0,false);clear_req(i);
 endrule;

  rule "L2 handles request"
    req[i].valid & !(req[i].t=RQ_PUTS | req[i].t=RQ_PUTE | req[i].t=RQ_PUTM) & l2s != L2_SPM_WB_INV &

    ( (req[i].t = RQ_PUTX &
        !(l2s = L2_MT_IIB | l2s = L2_MT_SPMS | l2s = L2_MT_MB |
          l2s = L2_SS_MB | l2s = L2_MT_I | l2s = L2_MCT_I) &

        !(IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_SPM_WB))
    | (req[i].t != RQ_PUTX &
        !(l2s = L2_SS_MB | l2s = L2_MT_MB | l2s = L2_MT_IIB |
          l2s = L2_MT_IB | l2s = L2_MT_SB | l2s = L2_MT_SPMS |
          l2s = L2_M_I | l2s = L2_MT_I | l2s = L2_MCT_I |
          l2s = L2_I_I | l2s = L2_S_I) &
        !( (l2s = L2_IS | l2s = L2_ISS) &
           (req[i].t = RQ_SILENT | req[i].t = RQ_SPMWB | req[i].t = RQ_GETX |
            (req[i].t = RQ_UPG & !l2_sharer[req[i].src])) ) &
        !( l2s = L2_IM &
           (req[i].t = RQ_GETS | req[i].t = RQ_GETX |
            (req[i].t = RQ_UPG & !l2_sharer[req[i].src])) ) &

        !( IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_IM &
           (req[i].t = RQ_SILENT | req[i].t = RQ_SPMWB) ) &
        !( l2s = L2_SPM_IS &
           (req[i].t = RQ_GETS | req[i].t = RQ_GETX | req[i].t = RQ_UPG |
            req[i].t = RQ_SILENT | req[i].t = RQ_SPMWB) ) &
        !( l2s = L2_SPM_WB &
           (req[i].t = RQ_GETS | req[i].t = RQ_GETX | req[i].t = RQ_UPG |
            req[i].t = RQ_SILENT | req[i].t = RQ_SPMWB) ) )
    )
  ==>
  var c: CoreT; v: ValT; d: boolean; is_upg_sharer: boolean; a: AckT;
      n: PendT; was_silent:boolean; was_sharer:boolean; was_owner:MaybeCore;
  begin
    c := req[i].src; v := req[i].val; d := req[i].dirty;
    was_silent := req[i].t=RQ_SILENT;was_sharer:=l2_sharer[c];was_owner:=l2_excl;
    is_upg_sharer := (req[i].t = RQ_UPG) & l2_sharer[c];

    if req[i].t = RQ_PUTX then
      clear_req(i);

      if l2_sharer[c] then
        if l2s = L2_MT then

          for o: CoreT do l2_sharer[o] := false; endfor;
          if d then l2_data := v; l2_dirty := true; endif;
          push_rspc(c, RC_WBACK, 0, false, 0, false);
          l2s := L2_M;
        elsif l2s = L2_NP | l2s = L2_IS | l2s = L2_ISS | l2s = L2_IM |
              l2s = L2_SS | l2s = L2_M | l2s = L2_M_I | l2s = L2_I_I |
              l2s = L2_S_I | l2s = L2_MT_IB | l2s = L2_MT_SB |
              (IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_SPM_IS) then

          push_rspc(c, RC_WBACK, 0, false, 0, false);
        else

          error "L2: L1_PUTX in a state with no SLICC transition";
        endif;
      else
        if l2s = L2_NP | l2s = L2_SS | l2s = L2_M | l2s = L2_MT |
           l2s = L2_M_I | l2s = L2_I_I | l2s = L2_S_I | l2s = L2_IS |
           l2s = L2_ISS | l2s = L2_IM | l2s = L2_MT_IB | l2s = L2_MT_SB |
           (IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_SPM_IS) then

          push_rspc(c, RC_WBACK, 0, false, 0, false);
        else
          error "L2: L1_PUTX_old in a state with no SLICC transition";
        endif;
      endif;

    elsif req[i].t = RQ_GETS then
      clear_req(i);
      if l2s = L2_NP then
        l2_dealloc();
        l2_sharer[c] := true;
        l2_clear_tbe();
        l2t_gets[c] := true;
        push_dreq(DQ_GETS, 0);
        l2s := L2_ISS;
      elsif l2s = L2_IS | l2s = L2_ISS then
        l2_sharer[c] := true;
        l2t_gets[c] := true;
        l2s := L2_IS;
      elsif l2s = L2_SS then
        push_rspc(c, RC_DATA, l2_data, false, 0, false);
        l2_sharer[c] := true;
      elsif l2s = L2_M then
        push_rspc(c, RC_DATAX, l2_data, false, 0, false);
        l2s := L2_MT_MB;
      elsif l2s = L2_MT then
        push_fwd(l2_excl, FW_GETS, false, c);
        l2s := L2_MT_IIB;
      else
        error "L2: L1_GETS in a state with no SLICC transition";
      endif;

    elsif req[i].t = RQ_GETX | (req[i].t = RQ_UPG & !is_upg_sharer) then
      clear_req(i);
      if l2s = L2_NP then
        l2_dealloc();
        l2_clear_tbe();
        l2t_getx := c;
        push_dreq(DQ_GETS, 0);
        l2s := L2_IM;
      elsif l2s = L2_SS then

        a := 0 - sharer_count();
        if l2_sharer[c] then a := a + 1; endif;
        push_rspc(c, RC_DATA, l2_data, false, a, false);
        for o: CoreT do
          if l2_sharer[o] & o != c then
            push_fwd(o, FW_INV, false, c);
          endif;
        endfor;
        l2s := L2_SS_MB;
      elsif l2s = L2_M then
        push_rspc(c, RC_DATA, l2_data, false, 0, false);
        l2s := L2_MT_MB;
      elsif l2s = L2_MT then
        push_fwd(l2_excl, FW_GETX, false, c);
        l2s := L2_MT_MB;
      else
        error "L2: L1_GETX in a state with no SLICC transition";
      endif;

    elsif req[i].t = RQ_UPG then
      clear_req(i);

      if l2s = L2_SS then
        a := 1 - sharer_count();
        push_rspc(c, RC_ACK, 0, false, a, false);
        for o: CoreT do
          if l2_sharer[o] & o != c then
            push_fwd(o, FW_INV, false, c);
          endif;
        endfor;
        l2s := L2_SS_MB;
      else
        error "L2: L1_UPGRADE in a state with no SLICC transition";
      endif;

    elsif req[i].t = RQ_SILENT then
      clear_req(i);
      if l2s = L2_NP then
        l2_dealloc();
        l2_clear_tbe();
        l2t_req := c;
        if RETAIN_LLC=1 then push_dreq(DQ_GETS,0);else push_dreq(DQ_SILENT,0);endif;
        l2s := L2_SPM_IS;
      elsif l2s = L2_SS then

        push_rspc(c, RC_DATA, l2_data, false, 0, false);
        if M_SILENT_REGISTERS=1 then l2_sharer[c] := true; endif;
      elsif l2s = L2_M then
        push_rspc(c, RC_DATA, l2_data, false, 0, false);
      elsif l2s = L2_MT & M_SILENT_STALE_OWNER=1 then
        push_rspc(c, RC_DATA, l2_data, false, 0, false);
      elsif l2s = L2_MT then
        push_fwd(l2_excl, FW_SILENT, false, c);
        if FNF_SILENT = 0 then
          l2s := L2_MT_SPMS;
        endif;
      else
        error "L2: SPM_GETS_SILENT in a state with no SLICC transition";
      endif;

    else

      clear_req(i);
      if l2s = L2_NP | l2s = L2_M | WB_INVALIDATE=0 then
        l2_clear_tbe(); l2t_req := c; l2t_data := v;
        commit_spm_writeback();
      elsif l2s = L2_SS | l2s = L2_MT then
        l2_clear_tbe(); l2t_req := c; l2t_data := v;
        l2t_pend := sharer_count();

        for o: CoreT do
          if l2_sharer[o] then push_fwd(o,FW_INV,true,c); endif;
        endfor;
        l2s := L2_SPM_WB_INV;
        if M_EARLY_WB_COMMIT=1 then l2t_pend := 0; commit_spm_writeback(); endif;
      else error "SPM writeback accepted in invalid state";
      endif;
    endif;
    if was_silent then
      assert l2_sharer[c]=was_sharer & l2_excl=was_owner
        "silent request changed coherent registration";
    endif;
  endrule;

endruleset;

ruleset i: Rsp2Idx do

  rule "L2 handles response"
    rsp2[i].valid
  ==>
  var t: Rsp2TypeT; c: CoreT; v: ValT; d: boolean;
  begin
    t := rsp2[i].t; c := rsp2[i].src; v := rsp2[i].val; d := rsp2[i].dirty;
    clear_rsp2(i);

    if l2s = L2_SPM_WB_INV & (t = R2_ACK | t = R2_WBDATA) then
      assert l2t_pend > 0 "duplicate writeback invalidation response";
      l2t_pend := l2t_pend-1;
      if l2t_pend = 0 then commit_spm_writeback(); endif;
    elsif t = R2_WBDATA then

      if l2s = L2_MT_IIB then
        l2_data := v; if d then l2_dirty := true; endif;
        l2s := L2_MT_SB;
      elsif l2s = L2_MT_IB then
        l2_data := v; if d then l2_dirty := true; endif;
        l2s := L2_SS;
      elsif l2s = L2_MT_I then
        if d then
          l2t_data := v; l2t_dirty := true;
          push_d2d(DD_DATA, v, true);
        else
          push_d2d(DD_DATA, l2t_data, l2t_dirty);
        endif;
        l2s := L2_M_I;
      elsif l2s = L2_MCT_I then
        if d then
          l2t_data := v; l2t_dirty := true;
          push_d2d(DD_DATA, v, true);
        else
          push_d2d(DD_ACK, 0, false);
        endif;
        l2s := L2_M_I;
      elsif IMPLEMENTED_TRANSITIONS = 1 & l2s = L2_MT_SPMS then

        l2_data := v;
        if d then l2_dirty := true; endif;
        for o: CoreT do l2_sharer[o] := false; endfor;
        l2s := L2_M;
      else
        error "L2: WB_Data in a state with no SLICC transition";
      endif;

    elsif t = R2_ACK then

      if l2s = L2_I_I then
        if l2t_pend = 1 then
          push_d2d(DD_ACK, 0, false);
          l2s := L2_M_I;
        else
          l2t_pend := l2t_pend - 1;
        endif;
      elsif l2s = L2_S_I then
        if l2t_pend = 1 then
          push_d2d(DD_DATA, l2t_data, l2t_dirty);
          l2s := L2_M_I;
        else
          l2t_pend := l2t_pend - 1;
        endif;
      elsif l2s = L2_MT_I then
        if l2t_pend = 1 then
          push_d2d(DD_DATA, l2t_data, l2t_dirty);
          l2s := L2_M_I;
        else
          l2t_pend := l2t_pend - 1;
        endif;
      elsif l2s = L2_MCT_I then
        if l2t_pend = 1 then
          push_d2d(DD_ACK, 0, false);
          l2s := L2_M_I;
        else
          l2t_pend := l2t_pend - 1;
        endif;
      else
        error "L2: Ack in a state with no SLICC transition";
      endif;

    elsif t = R2_MEMDATA then
      if l2s = L2_ISS then
        l2_data := v; l2_dirty := false;
        for o: CoreT do
          if l2t_gets[o] then
            push_rspc(o, RC_DATAX, v, false, 0, false);
          endif;
        endfor;
        l2_clear_tbe();
        l2s := L2_MT_MB;
      elsif l2s = L2_IS then
        l2_data := v; l2_dirty := false;
        for o: CoreT do
          if l2t_gets[o] then
            push_rspc(o, RC_DATA, v, false, 0, false);
          endif;
        endfor;
        l2_clear_tbe();
        l2s := L2_SS;
      elsif l2s = L2_IM then
        l2_data := v; l2_dirty := false;
        push_rspc(l2t_getx, RC_DATA, v, false, 0, false);
        l2_clear_tbe();
        l2s := L2_MT_MB;
      elsif l2s = L2_SPM_IS then

        push_rspc(l2t_req, RC_DATA, v, false, 0, false);
        if RETAIN_LLC=1 then l2_data:=v;l2_dirty:=false;l2s:=L2_M;
        else l2_dealloc();l2s:=L2_NP;endif;
        l2_clear_tbe();
      else
        error "L2: Mem_Data in a state with no SLICC transition";
      endif;

    elsif t = R2_MEMACK then
      if l2s = L2_M_I then
        l2_clear_tbe();
        l2s := L2_NP;
      else
        error "L2: Mem_Ack in a state with no SLICC transition";
      endif;

    else

      if l2s = L2_SPM_WB then
        push_rspc(l2t_req, RC_SPMWBACK, 0, false, 0, false);
        l2_clear_tbe();
        l2s := L2_M;
      else
        error "L2: SPMWB_Ack in a state with no SLICC transition";
      endif;
    endif;
  endrule;

endruleset;

rule "L2 handles MEM_Inv"
  dir_inv > 0 &
  (l2s = L2_NP | l2s = L2_SS | l2s = L2_M | l2s = L2_MT |
   l2s = L2_I_I | l2s = L2_S_I | l2s = L2_M_I | l2s = L2_MT_I |
   l2s = L2_MCT_I)
==>
begin
  dir_inv := dir_inv - 1;
  if l2s = L2_SS then
    l2_clear_tbe();
    l2t_data := l2_data; l2t_dirty := l2_dirty;
    l2t_pend := sharer_count();
    for o: CoreT do
      if l2_sharer[o] then push_fwd(o, FW_INV, true, 0); endif;
    endfor;
    l2_dealloc();
    l2s := L2_S_I;
  elsif l2s = L2_M then
    l2_clear_tbe();
    push_d2d(DD_DATA, l2_data, l2_dirty);
    l2_dealloc();
    l2s := L2_M_I;
  elsif l2s = L2_MT then
    l2_clear_tbe();
    l2t_data := l2_data; l2t_dirty := l2_dirty;
    l2t_pend := sharer_count();
    for o: CoreT do
      if l2_sharer[o] then push_fwd(o, FW_INV, true, 0); endif;
    endfor;
    l2_dealloc();
    l2s := L2_MT_I;
  endif;

endrule;

ruleset i: UnbIdx do

  rule "L2 handles unblock"
    unb[i].valid
  ==>
  var t: UnbTypeT; c: CoreT;
  begin
    t := unb[i].t; c := unb[i].src;
    clear_unb(i);
    if t = UB_XUNB then
      if l2s = L2_SS_MB | l2s = L2_MT_MB then
        for o: CoreT do l2_sharer[o] := false; endfor;
        l2_excl := c;
        l2_sharer[c] := true;
        l2s := L2_MT;
      else
        error "L2: Exclusive_Unblock in a state with no SLICC transition";
      endif;
    else
      if l2s = L2_MT_IIB then
        l2_sharer[c] := true;
        l2s := L2_MT_IB;
      elsif l2s = L2_MT_SB then
        l2_sharer[c] := true;
        l2s := L2_SS;
      elsif l2s = L2_MT_SPMS then

        l2s := L2_MT;
      else
        error "L2: Unblock in a state with no SLICC transition";
      endif;
    endif;
  endrule;

endruleset;

rule "L2 replacement SS"
  l2s = L2_SS & sharer_count()>0
==>
begin
  l2_clear_tbe();
  l2t_data := l2_data; l2t_dirty := l2_dirty;
  l2t_pend := sharer_count();
  for o: CoreT do
    if l2_sharer[o] then push_fwd(o, FW_INV, true, 0); endif;
  endfor;
  if l2_dirty then
    l2s := L2_S_I;
  else
    l2s := L2_I_I;
  endif;
  l2_dealloc();
endrule;

rule "L2 replacement M"
  (l2s = L2_M | (l2s=L2_SS & sharer_count()=0))
==>
begin
  l2_clear_tbe();
  if l2_dirty then
    push_d2d(DD_DATA, l2_data, true);
  else
    push_d2d(DD_ACK, 0, false);
  endif;
  l2_dealloc();
  l2s := L2_M_I;
endrule;

rule "L2 replacement MT"
  l2s = L2_MT
==>
begin
  l2_clear_tbe();
  l2t_data := l2_data; l2t_dirty := l2_dirty;
  l2t_pend := sharer_count();
  for o: CoreT do
    if l2_sharer[o] then push_fwd(o, FW_INV, true, 0); endif;
  endfor;
  if l2_dirty then
    l2s := L2_MT_I;
  else
    l2s := L2_MCT_I;
  endif;
  l2_dealloc();
endrule;

ruleset i: DReqIdx do

  rule "dir M sends INV for parked fetch"
    dreq[i].valid & dirs = D_M & !dreq[i].inv_sent &
    (dreq[i].t = DQ_GETS | dreq[i].t = DQ_SILENT)
  ==>
  begin
    dreq[i].inv_sent := true;
    if dir_inv = 2 then error "dir INV overflow"; endif;
    dir_inv := dir_inv + 1;
  endrule;

  rule "dir handles request"
    dreq[i].valid &
    ( (dreq[i].t = DQ_SPMWB & (dirs = D_I | dirs = D_M))
    | ((dreq[i].t = DQ_GETS | dreq[i].t = DQ_SILENT) & dirs = D_I) )
  ==>
  var t: DReqTypeT; v: ValT;
  begin
    t := dreq[i].t; v := dreq[i].val;
    clear_dreq(i);
    if memq != MQ_NONE then error "memory queue overflow"; endif;
    if t = DQ_GETS then
      memq := MQ_READ; memq_val := 0;
      dirs := D_IM;
    elsif t = DQ_SILENT then
      memq := MQ_READ; memq_val := 0;
      dirs := D_SPM_IS;
    else
      memq := MQ_WB; memq_val := v;
      dirs := D_SPM_WB;
    endif;
  endrule;

endruleset;

ruleset i: D2DIdx do

  rule "dir handles L2 response"
    d2d[i].valid &
    ( (d2d[i].t = DD_DATA & dirs = D_M)
    | (d2d[i].t = DD_ACK  & dirs = D_M) )
  ==>
  var t: D2DTypeT; v: ValT;
  begin
    t := d2d[i].t; v := d2d[i].val;
    clear_d2d(i);
    if t = DD_DATA then
      if memq != MQ_NONE then error "memory queue overflow"; endif;
      memq := MQ_WB; memq_val := v;
      dirs := D_MI;
    else

      push_rsp2(R2_MEMACK, 0, 0, false);
      dirs := D_I;
    endif;
  endrule;

  rule "dir CleanReplacement undefined in transients"
    d2d[i].valid & d2d[i].t = DD_ACK &
    !(dirs = D_M | dirs = D_I) &
    !(CLEAN_FIX = 1 & (dirs = D_MDRD | dirs = D_MDWR))
  ==>
  begin
    error "dir: CleanReplacement in a state with no SLICC transition";
  endrule;

endruleset;

rule "dma issue read"
  DMA_OPS > 0 & dma_used < DMA_OPS & !dma_busy
==>
begin
  dma_used := dma_used + 1; dma_busy := true; dma_req := DMA_RD;
endrule;

rule "dma issue write"
  DMA_OPS > 0 & dma_used < DMA_OPS & !dma_busy
==>
begin
  dma_used := dma_used + 1; dma_busy := true; dma_req := DMA_WR;
endrule;

-- SLICC recycles DMA in ID, ID_W, M_DRD, M_DRDI, M_DWR, M_DWRI, IM, MI (dir.sm:681).
-- SPM_IS/SPM_WB (and SPM_PUT_WB, not reachable under retention) have no DMA transition.
rule "dir handles DMA request"
  dma_req != DMA_NONE &
  !(dirs = D_IM | dirs = D_MI | dirs = D_ID | dirs = D_IDW | dirs = D_MDRD |
    dirs = D_MDRDI | dirs = D_MDWR | dirs = D_MDWRI | dirs = D_MDRD_SWB |
    dirs = D_MDWR_SWB) &
  !(DMA_STALL_SPM = 1 & (dirs = D_SPM_IS | dirs = D_SPM_WB))
==>
var w: ValT;
begin
  if dirs = D_I then
    if memq != MQ_NONE then error "memory queue overflow"; endif;
    if dma_req = DMA_RD then
      memq := MQ_READ; memq_val := 0; dirs := D_ID;
    else
      -- Ordered before an SPM writeback still in flight to the directory.
      if spmwb_inflight() then w := inc_val(pre_wb); pre_wb := w;
      else w := inc_val(latest); ghost_store(w); endif;
      memq := MQ_WB; memq_val := w; dirs := D_IDW;
    endif;
  elsif dirs = D_M then
    if dir_inv = 2 then error "dir INV overflow"; endif;
    dir_inv := dir_inv + 1;
    if dma_req = DMA_RD then dirs := D_MDRD; else dirs := D_MDWR; endif;
  else
    error "dir: DMA request in a state with no SLICC transition";
  endif;
  dma_req := DMA_NONE;
endrule;

ruleset i: D2DIdx do
  rule "dir DMA receives L2 data"
    d2d[i].valid & d2d[i].t = DD_DATA & (dirs = D_MDRD | dirs = D_MDWR)
  ==>
  var v: ValT; w: ValT;
  begin
    v := d2d[i].val; clear_d2d(i);
    if memq != MQ_NONE then error "memory queue overflow"; endif;
    if dirs = D_MDRD then
      assert v = latest "DMA read (LLC data) returned a stale value";
      dma_busy := false;
      memq := MQ_WB; memq_val := v; dirs := D_MDRDI;
    else
      w := inc_val(latest); ghost_store(w);
      memq := MQ_WB; memq_val := w; dirs := D_MDWRI;
    endif;
  endrule;
endruleset;

ruleset i: D2DIdx do
  rule "dir DMA receives CleanReplacement"
    CLEAN_FIX = 1 & d2d[i].valid & d2d[i].t = DD_ACK & (dirs = D_MDRD | dirs = D_MDWR)
  ==>
  var w: ValT;
  begin
    clear_d2d(i);
    push_rsp2(R2_MEMACK, 0, 0, false);
    if memq != MQ_NONE then error "memory queue overflow"; endif;
    if dirs = D_MDRD then
      memq := MQ_READ; memq_val := 0; dirs := D_ID;
    else
      w := inc_val(latest); ghost_store(w);
      memq := MQ_WB; memq_val := w; dirs := D_IDW;
    endif;
  endrule;
endruleset;

-- Fresh (not already parked) requests reaching M_DRD/M_DWR: no SLICC transition.
ruleset i: DReqIdx do
  rule "dir request during DMA invalidation"
    dreq[i].valid & !dreq[i].inv_sent & (dirs = D_MDRD | dirs = D_MDWR) &
    (SWB_IN_DRD = 0 | (SWB_IN_DRD = 2 & dreq[i].t = DQ_SPMWB))
  ==>
  var t: DReqTypeT; v: ValT;
  begin
    t := dreq[i].t; v := dreq[i].val;
    if SWB_IN_DRD = 0 then
      error "dir: request during DMA invalidation has no SLICC transition";
    endif;
    clear_dreq(i);
    if memq != MQ_NONE then error "memory queue overflow"; endif;
    memq := MQ_WB; memq_val := v;
    if dirs = D_MDRD then dirs := D_MDRD_SWB; else dirs := D_MDWR_SWB; endif;
  endrule;
endruleset;

rule "memory completes"
  memq != MQ_NONE
==>
begin
  if memq = MQ_READ then
    memq := MQ_NONE;
    if dirs = D_IM then
      push_rsp2(R2_MEMDATA, 0, mem_data, false);
      dirs := D_M;
    elsif dirs = D_SPM_IS then
      push_rsp2(R2_MEMDATA, 0, mem_data, false);
      dirs := D_I;
    elsif dirs = D_ID then
      if spmwb_inflight() then
        assert mem_data = pre_wb "DMA read (memory) returned a stale value";
      else
        assert mem_data = latest "DMA read (memory) returned a stale value";
      endif;
      dma_busy := false;
      dirs := D_I;
    else
      error "dir: Memory_Data in a state with no SLICC transition";
    endif;
  else
    mem_data := memq_val;
    memq := MQ_NONE;
    memq_val := 0;
    if dirs = D_MI then
      push_rsp2(R2_MEMACK, 0, 0, false);
      dirs := D_I;
    elsif dirs = D_SPM_WB then
      push_rsp2(R2_SPMWBACK, 0, 0, false);
      dirs := D_M;
    elsif dirs = D_IDW then
      dma_busy := false;
      dirs := D_I;
    elsif dirs = D_MDRDI then
      push_rsp2(R2_MEMACK, 0, 0, false);
      dirs := D_I;
    elsif dirs = D_MDWRI then
      push_rsp2(R2_MEMACK, 0, 0, false);
      dma_busy := false;
      dirs := D_I;
    elsif dirs = D_MDRD_SWB then
      push_rsp2(R2_SPMWBACK, 0, 0, false);
      dirs := D_MDRD;
    elsif dirs = D_MDWR_SWB then
      push_rsp2(R2_SPMWBACK, 0, 0, false);
      dirs := D_MDWR;
    else
      error "dir: Memory_Ack in a state with no SLICC transition";
    endif;
  endif;
endrule;

invariant "SWMR"
  forall c1: CoreT do
    (l1s[c1] = L1_E | l1s[c1] = L1_EE | l1s[c1] = L1_M | l1s[c1] = L1_MM |
     l1s[c1] = L1_E_IL0 | l1s[c1] = L1_M_IL0 | l1s[c1] = L1_MM_IL0 |
     l1s[c1] = L1_EX_L0 | l1s[c1] = L1_MX_L0)
    ->
    forall c2: CoreT do
      c2 = c1 |
      !(l1s[c2] = L1_S | l1s[c2] = L1_SS | l1s[c2] = L1_E | l1s[c2] = L1_EE |
        l1s[c2] = L1_M | l1s[c2] = L1_MM | l1s[c2] = L1_SM |
        l1s[c2] = L1_S_IL0 | l1s[c2] = L1_E_IL0 | l1s[c2] = L1_M_IL0 |
        l1s[c2] = L1_MM_IL0 | l1s[c2] = L1_SM_IL0 |
        l1s[c2] = L1_SX_L0 | l1s[c2] = L1_EX_L0 | l1s[c2] = L1_MX_L0)
    endforall
  endforall;

invariant "shared copies fresh"
  forall c: CoreT do
    ((l1s[c] = L1_S | l1s[c] = L1_SS | l1s[c] = L1_S_IL0 |
      l1s[c] = L1_SX_L0 | l1s[c] = L1_SM | l1s[c] = L1_SM_IL0)
      -> l1_data[c] = latest)
    &
    (l0_present[c] & l0_dirty[c] -> l0_data[c] = latest)
    &
    (l0_present[c] & !l0_dirty[c] & in_l0_cache(l1s[c])
      -> l0_data[c] = latest)
    &
    ((l1s[c] = L1_E | l1s[c] = L1_EE | l1s[c] = L1_M | l1s[c] = L1_MM |
      l1s[c] = L1_E_IL0 | l1s[c] = L1_M_IL0 | l1s[c] = L1_MM_IL0 |
      l1s[c] = L1_EX_L0 | l1s[c] = L1_MX_L0)
      & !(l0_present[c] & l0_dirty[c]) & l0q_cnt[c] = 0 & !l0_recall[c]
      -> l1_data[c] = latest)
  endforall;

invariant "L2 shared data fresh"
   (l2s = L2_SS -> l2_data = latest);

invariant "L2 exclusive-free data fresh"
   (l2s = L2_M -> l2_data = latest);

invariant "SPM slot isolation"
  forall c: CoreT do
    (spm_st[c] = SP_X | spm_st[c] = SP_XWB) -> spm_data[c] = spm_expected[c]
  endforall;

invariant "install migration never hits the no-free-way panic"
  no_free_way -> false;

invariant "L1 set never oversubscribed"
  forall c: CoreT do
    (line_resident(c) -> spm_ways(c) + 1 <= WAYS) &
    (!line_resident(c) -> spm_ways(c) <= WAYS)
  endforall;

invariant "contract keeps MIN_FREE non-SPM ways"
  forall c: CoreT do
    spm_ways(c) <= WAYS - MIN_FREE
  endforall;

invariant "L2 MT has a plausible owner"
  (l2s = L2_MT ->
    (l2_excl != NCORE &
     !(l1s[l2_excl] = L1_S | l1s[l2_excl] = L1_SS |
       l1s[l2_excl] = L1_S_IL0 | l1s[l2_excl] = L1_SX_L0)));

invariant "L2 stable-shared implies no exclusive L1"
  (l2s = L2_SS ->
    forall c: CoreT do
      !(l1s[c] = L1_E | l1s[c] = L1_EE | l1s[c] = L1_M | l1s[c] = L1_MM |
        l1s[c] = L1_E_IL0 | l1s[c] = L1_M_IL0 | l1s[c] = L1_MM_IL0 |
        l1s[c] = L1_EX_L0 | l1s[c] = L1_MX_L0)
    endforall);

invariant "quiescent memory fresh"
  ( ( dirs = D_I & l2s = L2_NP & memq = MQ_NONE &
      forall c: CoreT do
        l1s[c] = L1_I & !l0_present[c] & l0q_cnt[c] = 0 &
        forall i: FwdIdx do !fwd[c][i].valid endforall &
        forall i: RspCIdx do !rspc[c][i].valid endforall
      endforall &
      forall i: ReqIdx do !req[i].valid endforall &
      forall i: Rsp2Idx do !rsp2[i].valid endforall &
      forall i: DReqIdx do !dreq[i].valid endforall &
      forall i: D2DIdx do !d2d[i].valid endforall )
    -> mem_data = latest );

liveness "all cores can drain"
  forall c: CoreT do cpu_op[c] = OP_NONE endforall & !dma_busy & dma_req = DMA_NONE;

invariant "retry retains original miss transaction"
 forall c: CoreT do retry_pending[c] -> (l1s[c] = L1_IX_D & cpu_op[c] = OP_CPF) endforall;
invariant "relinquishing source cannot serve a new CPU operation"
 forall c:CoreT do
 (l1s[c]=L1_SX_A | l1s[c]=L1_EX_A | l1s[c]=L1_MX_A | l1s[c]=L1_IX_A)
 -> (cpu_op[c]=OP_CPF & cpu_busy[c]) endforall;

invariant "snapshot buffer belongs to outstanding copy"
 forall c:CoreT do capture_pending[c] -> (cpu_op[c]=OP_CPF & cpu_busy[c]) endforall;

invariant "retained-source copy never sends release PUT"
 RETAIN_SOURCE=1 -> forall i:ReqIdx do
 !(req[i].valid & (req[i].t=RQ_PUTS | req[i].t=RQ_PUTE | req[i].t=RQ_PUTM)) endforall;
