-- CacheFlex SPM multicore protocol model (Rumur/Murphi).
--
-- Models the CURRENT copy/snapshot implementation of the CacheFlex SPM
-- extension to gem5 MESI_Three_Level, at message-passing granularity:
--
--   * NCORE private-L1 controllers (gem5 "L1Cache", the SPM-holding level),
--     each with an abstracted core-side L0 cache and one private SPM slot
--   * one shared L2/LLC bank (gem5 "L2Cache", the on-chip directory)
--   * the memory directory (gem5 "Directory") plus main memory
--   * one coherent cache-line address; per-core SPM slot addresses
--
-- Sources of truth (SLICC):
--   gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-L1cache.sm
--   gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-L2cache.sm
--   gem5/src/mem/ruby/protocol/MESI_Three_Level_SPM-dir.sm
-- and the harvested tables spm_coherency_{L0,L1,dir}.csv.
--
-- Modeled SPM semantics (the current design):
--   * SPMCP_fetch = copy/snapshot: source line stays coherent.
--     - L1 source S/E/M   -> SX_L0/EX_L0/MX_L0 (recall L0 for freshness),
--                            then install SPM copy, source rests in SS/EE/MM
--     - L1 source SS/EE/MM -> install SPM copy immediately
--     - L1 source I        -> IX_D + GETS_SILENT; L2 serves from SS/M, forwards
--                            silently to an MT owner (MT_SPMS), or fetches from
--                            memory (SPM_IS at L2 and at the directory); the
--                            requester is never registered as sharer/owner
--   * SPMCP_install claims a zeroed slot (X); SPMLD/SPMST hit the X slot;
--     SPMWB_store pushes slot data through L2 (SPM_WB) and the directory
--     (SPM_WB) to memory; SPM_release drops the slot.
--
-- UNIFIED placement plane (single model, not a separate one): each core's L1
-- has a physical set of WAYS ways.  The coherent line and the SPM slots share
-- it, so SPMCP_install / SPMCP_fetch may land on the way holding the coherent
-- line and must LAZILY MIGRATE it to a free way (pure local placement -- the
-- directory is way-agnostic, so migration is coherence-neutral) under the
-- ">= MIN_FREE non-SPM ways" software contract.  This is verified TOGETHER
-- with live cross-core coherence traffic on the migrated line (see helpers
-- line_resident/free_ways, the two SPMCP_install modes, and findings F9 /
-- no-free-way).  It replaces the earlier standalone placement model.
--
-- Deliberately EXCLUDED (not reachable or out of scope in the current build):
--   * the legacy release path (PUTS/PUTM/PUTE, L1 SX_A/MX_A/EX_A, dir
--     SPM_PUT_WB): no SLICC transition enters *X_A from a stable state anymore
--   * flush (I_F/V_F/D_F, L2 flush states), HTM (PUTX_COPY/NAK), prefetch,
--     LL/SC, instruction fetch (GET_INSTR), DMA, multiple L2 banks
--
-- Abstractions:
--   * The L0 cache is folded into each core: presence/dirtiness/data of the
--     L0 copy plus an ordered L0->L1 FIFO carrying WriteBack (PUTX) and
--     recall responses (INV_DATA/INV_ACK).  This preserves the races the
--     SLICC L1 cares about (WriteBack vs recall, dirty-data freshness).
--     CPU requests are modeled as one outstanding op per core, delivered to
--     the L1 exactly in the states the SLICC accepts them (stall_and_wait
--     parks otherwise, which is what the guard-based delivery models).
--   * Network vnets are modeled as bounded UNORDERED message pools.  Ruby's
--     stall_and_wait/recycle reorder same-address messages anyway, so
--     unordered delivery is the honest contract.  L0<->L1 buffers are
--     ordered=True in MESI_Three_Level.py and are modeled as a FIFO.
--   * A (state,event) pair with no SLICC transition and no stall is a
--     protocol panic in gem5; the model raises an error for those, so the
--     checker decides reachability of every suspected "undefined transition".
--
-- Checked properties:
--   * SWMR + data-value coherence (ghost `latest`), guarded by `wb_race`
--     because SPMWB_store is architecturally a racy write when a foreign
--     coherent owner exists (software epoch contract)
--   * SPM slot isolation: coherent traffic never mutates a live X slot
--   * snapshot freshness: an SPMCP_fetch with no racing store installs the
--     globally latest value; silent fetches never register as sharer/owner
--   * directory/L2 metadata consistency in stable states
--   * liveness: every core can always eventually return to idle (this is the
--     property the PageRank "sequencer unanswered for 804K cycles" stampede
--     would violate)

const
  -- 0: reachable SLICC gaps (missing transitions) abort the search with an
  --    error trace, one finding at a time.
  -- 1: confirmed gaps listed under "FINDINGS" in the header behave as the
  --    natural fix (stall_and_wait) so verification can continue deeper.
  ASSUME_FIXES: 1;

  -- Separately togglable model of the software placement contract that an
  -- SPMCP_install target way never aliases a line with an outstanding
  -- coherence transaction.  1 enforces it; 0 lets install displace a transient
  -- line, reaching FINDING F9 (an IsStableCoherent assert).
  --
  -- NOTE (verified against current gem5, 2026-07-06): F9 reflects an EARLIER
  -- design.  The current CacheMemory.cc migrateOrClearSPMSlot has NO
  -- IsStableCoherent assert -- it relocates ANY live occupant (transient or
  -- stable) to a free way, which is safe because all cache state is
  -- address-keyed (looked up by tag, not way), so an outstanding transaction
  -- on a relocated line still finds it.  So F9 is NOT a live bug and needs no
  -- SLICC/C++ fix; this knob is retained only to reproduce the historical
  -- finding.  The real placement obligation that DOES bite is the no-free-way
  -- panic (the >= MIN_FREE capacity contract), modeled separately.
  INSTALL_STABLE_ONLY: 1;

  -- Bounded-complete search: cap the total number of CPU operations issued
  -- across all cores.  Because new coherence activity originates only at a
  -- CPU op (environment eviction/replacement rules only ever remove state),
  -- bounding STEPS makes the reachable state space finite and fully
  -- explorable, covering ALL interleavings of up to STEPS operations.
  -- 0 disables the cap (unbounded; intractable without symmetry reduction).
  -- Exhaustive-within-bound results (rumur, --deadlock-detection off, all
  -- coherence + placement safety invariants + the liveness drain property,
  -- MIN_FREE=2):
  --   NCORE=2: clean through STEPS=4 (~6.9M states)
  --   NCORE=3: clean through STEPS=3 (~7.7M states)
  -- run_cacheflex_spm.sh overrides STEPS/NCORE/MIN_FREE/etc for the sweep;
  -- this default is the NCORE=3, STEPS=3 regression (~7.7M states, ~70s).
  STEPS:        3;

  NCORE:      3;    -- cores; the GETS_SILENT stampede needs >= 3
  VMAX:       1;    -- data values 0..VMAX (stores flip/rotate the value)

  -- Physical L1 set/way dimension (unified placement + coherence).  Each core's
  -- private L1 has one modeled set of WAYS ways.  The coherent line and the SPM
  -- slots share that set, so SPMCP_install can land on the way holding the
  -- coherent line and must LAZILY MIGRATE it to a free way (pure local
  -- placement: state/data/directory-role preserved, no coherence message).
  -- PINNED models additional live SPM slots in the same set (beyond the one
  -- primary slot tracked by spm_st) so the way-capacity contract is exercised.
  -- MIN_FREE is the software contract: keep >= MIN_FREE non-SPM ("available")
  -- ways per set so a coherent fetch and a migration always have a home.
  WAYS:       3;
  MAXPIN:     2;    -- cap on modeled extra pinned SPM slots per core (<= WAYS-1)
  -- Software contract: keep >= MIN_FREE non-SPM ways per set.  In the UNIFIED
  -- model the coherent line itself occupies one non-SPM way, so the free ways
  -- available to a migration are (non-SPM ways) - (line resident).  Verified
  -- below: MIN_FREE=2 (the stated CacheFlex contract) is clean with one way of
  -- headroom; MIN_FREE=1 is the strict minimum; MIN_FREE=0 reaches the
  -- no-free-way migration panic.
  MIN_FREE:   2;
  REQ_POOL:   6;    -- L1->L2 request-vnet pool (>= 2 per core)
  FWD_POOL:   2;    -- L2->L1 forwarded-request pool, per core
  RSPC_POOL:  4;    -- responses to one core (data + up to NCORE-1 acks + ack)
  RSP2_POOL:  5;    -- responses to L2 (core WB/acks + one dir message)
  UNB_POOL:   3;    -- unblock messages to L2
  DREQ_POOL:  2;    -- L2->dir requests
  D2D_POOL:   2;    -- L2->dir responses (replacement data/clean ack)
  L0Q_DEPTH:  2;    -- ordered L0->L1 queue (WriteBack + recall response)

type
  CoreT:     0 .. NCORE-1;
  MaybeCore: 0 .. NCORE;               -- NCORE encodes "none"
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

  -- L1 controller states for the coherent source line
  -- (SLICC L1Cache_State_*; *X_A legacy states omitted as unreachable)
  L1StateT: enum {
    L1_I, L1_S, L1_SS, L1_E, L1_EE, L1_M, L1_MM,
    L1_IS, L1_IM, L1_SM, L1_IS_I, L1_M_I, L1_SINK,
    L1_S_IL0, L1_E_IL0, L1_M_IL0, L1_MM_IL0, L1_SM_IL0,
    L1_IX_D, L1_SX_L0, L1_EX_L0, L1_MX_L0
  };

  -- per-core private SPM slot state (SLICC states X / XWB on the slot addr)
  SlotT: enum { SP_INV, SP_X, SP_XWB };

  L2StateT: enum {
    L2_NP, L2_ISS, L2_IS, L2_IM, L2_SPM_IS, L2_SPM_WB,
    L2_SS, L2_M, L2_MT,
    L2_SS_MB, L2_MT_MB, L2_MT_IIB, L2_MT_IB, L2_MT_SB, L2_MT_SPMS,
    L2_M_I, L2_MT_I, L2_MCT_I, L2_I_I, L2_S_I
  };

  DirStateT: enum { D_I, D_IM, D_M, D_MI, D_SPM_IS, D_SPM_WB };

  MemOpT: enum { MQ_NONE, MQ_READ, MQ_WB };

  CpuOpT: enum { OP_NONE, OP_LD, OP_ST, OP_CPF, OP_INST,
                 OP_SLD, OP_SST, OP_SWB, OP_REL };

  -- L1 -> L2 requests (vnet 0)
  ReqTypeT: enum { RQ_GETS, RQ_GETX, RQ_UPG, RQ_PUTX, RQ_SILENT, RQ_SPMWB };
  ReqMsgT: record
    valid: boolean;
    t:     ReqTypeT;
    src:   CoreT;
    val:   ValT;
    dirty: boolean;
  end;

  -- L2 -> L1 forwarded requests (vnet 2); req = original requestor when the
  -- message was relayed on behalf of another core, req_is_l2 for L2-initiated
  FwdTypeT: enum { FW_INV, FW_GETX, FW_GETS, FW_SILENT };
  FwdMsgT: record
    valid:     boolean;
    t:         FwdTypeT;
    req_is_l2: boolean;
    req:       CoreT;
  end;

  -- responses to a core (vnet 1)
  RspCTypeT: enum { RC_DATA, RC_DATAX, RC_ACK, RC_WBACK, RC_SPMWBACK };
  RspCMsgT: record
    valid:   boolean;
    t:       RspCTypeT;
    val:     ValT;
    dirty:   boolean;
    ack:     AckT;      -- AckCount (negative from L2 data/upgrade-ack)
    from_l1: boolean;   -- sender machine type (DataS_fromL1 discrimination)
  end;

  -- responses to the L2 (vnet 1): core writeback data / inv acks, and
  -- directory MEMORY_DATA / MEMORY_ACK / SPMWB_ACK
  Rsp2TypeT: enum { R2_WBDATA, R2_ACK, R2_MEMDATA, R2_MEMACK, R2_SPMWBACK };
  Rsp2MsgT: record
    valid: boolean;
    t:     Rsp2TypeT;
    src:   CoreT;       -- meaningful for R2_WBDATA / R2_ACK
    val:   ValT;
    dirty: boolean;
  end;

  UnbTypeT: enum { UB_UNB, UB_XUNB };
  UnbMsgT: record
    valid: boolean;
    t:     UnbTypeT;
    src:   CoreT;
  end;

  -- L2 -> directory requests (vnet 0)
  DReqTypeT: enum { DQ_GETS, DQ_SILENT, DQ_SPMWB };
  DReqMsgT: record
    valid:    boolean;
    t:        DReqTypeT;
    val:      ValT;
    inv_sent: boolean;  -- dir M sent INV for this parked fetch already
  end;

  -- L2 -> directory responses (replacement MEMORY_DATA / clean ACK)
  D2DTypeT: enum { DD_DATA, DD_ACK };
  D2DMsgT: record
    valid: boolean;
    t:     D2DTypeT;
    val:   ValT;
    dirty: boolean;
  end;

  -- ordered L0 -> L1 queue entries: L0 dirty/clean eviction (WriteBack) and
  -- recall responses (INV_DATA dirty / INV_ACK clean-or-absent)
  L0MsgKindT: enum { L0M_WB, L0M_IACK, L0M_IDATA };
  L0MsgT: record
    kind:  L0MsgKindT;
    dirty: boolean;
    val:   ValT;
  end;

var
  -- per-core L1 controller (coherent source line)
  l1s:      array [CoreT] of L1StateT;
  l1_data:  array [CoreT] of ValT;
  l1_dirty: array [CoreT] of boolean;
  l1t_data:  array [CoreT] of ValT;    -- TBE (M_I / IM / SM / IX_D buffer)
  l1t_dirty: array [CoreT] of boolean;
  l1t_pend:  array [CoreT] of AckT;    -- pendingAcks

  -- per-core abstract L0
  l0_present: array [CoreT] of boolean;
  l0_dirty:   array [CoreT] of boolean;
  l0_data:    array [CoreT] of ValT;
  l0_recall:  array [CoreT] of boolean;      -- INV_OWN/INV_ELSE outstanding
  l0q:      array [CoreT] of array [L0QIdx] of L0MsgT;
  l0q_cnt:  array [CoreT] of L0QCnt;

  -- per-core SPM slot (the one primary slot whose data/lifecycle is tracked)
  spm_st:   array [CoreT] of SlotT;
  spm_data: array [CoreT] of ValT;

  -- per-core physical way occupancy of the modeled L1 set.  pinned = number of
  -- ADDITIONAL live SPM slots pinning ways (the primary spm_st slot pins one
  -- more when it is SP_X/SP_XWB).  Coherent-line residency is derived from the
  -- L1 coherence state (line_resident/line_stable below).  A migration ghost
  -- records that SPMCP_install actually displaced the coherent line.
  pinned:      array [CoreT] of 0 .. MAXPIN;
  saw_migrate: array [CoreT] of boolean;
  no_free_way: boolean;   -- SPMCP_install migration found no home (PANIC/F)

  -- per-core CPU
  cpu_op:   array [CoreT] of CpuOpT;
  cpu_busy: array [CoreT] of boolean;  -- op accepted by L1, completion pending

  -- shared L2 bank
  l2s:       L2StateT;
  l2_sharer: array [CoreT] of boolean;
  l2_excl:   MaybeCore;
  l2_data:   ValT;
  l2_dirty:  boolean;
  l2t_data:  ValT;                     -- L2 TBE
  l2t_dirty: boolean;
  l2t_pend:  PendT;
  l2t_req:   CoreT;                    -- SPM_IS / SPM_WB requester
  l2t_gets:  array [CoreT] of boolean; -- ISS/IS GetS requestors
  l2t_getx:  CoreT;

  -- memory directory + memory
  dirs:     DirStateT;
  mem_data: ValT;
  memq:     MemOpT;
  memq_val: ValT;
  dir_inv:  0 .. 2;                    -- INVs from dir to L2 in flight

  -- network pools
  req:  array [ReqIdx] of ReqMsgT;
  fwd:  array [CoreT] of array [FwdIdx] of FwdMsgT;
  rspc: array [CoreT] of array [RspCIdx] of RspCMsgT;
  rsp2: array [Rsp2Idx] of Rsp2MsgT;
  unb:  array [UnbIdx] of UnbMsgT;
  dreq: array [DReqIdx] of DReqMsgT;
  d2d:  array [D2DIdx] of D2DMsgT;

  -- ghosts
  latest:  ValT;                       -- last coherent value written
  wb_race: boolean;                    -- SPMWB applied over a live foreign copy
  steps_used: 0 .. 63;                 -- CPU ops issued so far (0..STEPS)
  stored_since_cpf: array [CoreT] of boolean;
  spm_expected:     array [CoreT] of ValT;
  -- snapshot of the requester's stale L2 registration at GETS_SILENT issue,
  -- so assert_silent checks the fetch ADDED nothing (a stale sharer left by a
  -- prior silent SS->I clean eviction is legitimate directory behavior)
  cpf_sh0: array [CoreT] of boolean;
  cpf_ow0: array [CoreT] of boolean;

---------------------------------------------------------------------------
-- helpers
---------------------------------------------------------------------------

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
  -- SLICC inL0Cache(): S, E, M, SM, S_IL0, E_IL0, M_IL0, SM_IL0
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

-- The coherent line A occupies a physical way whenever this L1 holds a cache
-- entry for it.  In SLICC that is every state except I and IX_D (SPMCP_fetch
-- from I routes straight to SPM and keeps A invalid in the coherence domain).
function line_resident(c: CoreT): boolean;
begin
  return !(l1s[c] = L1_I | l1s[c] = L1_IX_D);
end;

-- A stable coherent occupant is the only kind SPMCP_install may lazily migrate
-- (SLICC: allocateSPMSlot asserts IsStableCoherent on the displaced occupant).
function line_stable(c: CoreT): boolean;
begin
  return l1s[c] = L1_S | l1s[c] = L1_SS | l1s[c] = L1_E |
         l1s[c] = L1_EE | l1s[c] = L1_M | l1s[c] = L1_MM;
end;

-- ways this core's set currently pins for SPM (primary slot + extra pinned)
function spm_ways(c: CoreT): 0 .. MAXPIN+1;
begin
  if spm_st[c] = SP_INV then
    return pinned[c];
  else
    return pinned[c] + 1;
  endif;
end;

-- free (non-SPM, non-line) ways available for a fetch or a migration target
function free_ways(c: CoreT): 0 .. WAYS;
var used: 0 .. WAYS;
begin
  used := spm_ways(c);
  if line_resident(c) then used := used + 1; endif;
  return WAYS - used;
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

-- a coherent store retires globally: bump ghost `latest`
procedure ghost_store(v: ValT);
begin
  latest := v;
  for o: CoreT do
    stored_since_cpf[o] := true;
  endfor;
end;

-- SPMCP snapshot lands in core c's slot
procedure spm_install(c: CoreT; v: ValT);
begin
  spm_st[c] := SP_X;
  spm_data[c] := v;
  spm_expected[c] := v;
  -- freshness: a fetch that raced no store must observe the latest value
  if !stored_since_cpf[c] & !wb_race then
    assert v = latest "SPMCP snapshot missed the latest coherent value";
  endif;
end;

-- SPMCP completion must not have NEWLY registered the requester in the
-- directory (cf. the paper claim: GETS_SILENT adds no sharer, so no
-- invalidation storm).  A pre-existing stale entry from a prior silent
-- clean eviction is allowed; the fetch must not create one.
procedure assert_silent(c: CoreT);
begin
  assert (!l2_sharer[c] | cpf_sh0[c])
    "silent SPM fetch registered requester as L2 sharer";
  -- Exclusive is stale metadata outside the owner-authoritative states
  if l2s = L2_MT | l2s = L2_MT_IIB |
     l2s = L2_MT_IB | l2s = L2_MT_SB | l2s = L2_MT_SPMS then
    assert (l2_excl != c | cpf_ow0[c])
      "silent SPM fetch registered requester as L2 owner";
  endif;
end;

-- CPU op done
procedure cpu_done(c: CoreT);
begin
  cpu_op[c] := OP_NONE;
  cpu_busy[c] := false;
end;

-- store retires at core c (write permission is already held)
procedure apply_store(c: CoreT);
begin
  l0_present[c] := true;
  l0_dirty[c] := true;
  l0_data[c] := inc_val(latest);
  ghost_store(l0_data[c]);
  cpu_done(c);
end;

-- L1 recall of the L0 copy (forward_eviction_to_L0_own/_else)
procedure send_recall(c: CoreT);
begin
  l0_recall[c] := true;
end;

-- L2 allocates/deallocates
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

-- SPMWB_store is an incoherent write: it updates L2/memory without
-- invalidating coherent copies or in-flight fills.  The software epoch
-- contract requires the source line to be completely quiet.  A writeback
-- into a non-quiet line sets the sticky ghost wb_race, which switches the
-- data-value invariants off on that path (the interleavings themselves
-- remain fully explored for reachability of protocol panics/deadlocks).
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
    -- other SPM writebacks serialize behind this one and are fine
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

---------------------------------------------------------------------------
-- start state
---------------------------------------------------------------------------

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
    cpu_op[c] := OP_NONE; cpu_busy[c] := false;
    stored_since_cpf[c] := false; spm_expected[c] := 0;
    cpf_sh0[c] := false; cpf_ow0[c] := false;
    for i: FwdIdx do clear_fwd(c, i); endfor;
    for i: RspCIdx do clear_rspc(c, i); endfor;
  endfor;
  l2s := L2_NP; l2_dealloc(); l2_clear_tbe();
  dirs := D_I; mem_data := 0; memq := MQ_NONE; memq_val := 0; dir_inv := 0;
  for i: ReqIdx do clear_req(i); endfor;
  for i: Rsp2Idx do clear_rsp2(i); endfor;
  for i: UnbIdx do clear_unb(i); endfor;
  for i: DReqIdx do clear_dreq(i); endfor;
  for i: D2DIdx do clear_d2d(i); endfor;
  latest := 0; wb_race := false; steps_used := 0; no_free_way := false;
endstartstate;

---------------------------------------------------------------------------
-- CPU: issue one outstanding op per core
---------------------------------------------------------------------------

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

  -- SPMCP_fetch also installs its snapshot into the primary SPM slot, which
  -- pins a way if the slot was empty; only issue it when the >= MIN_FREE
  -- contract leaves room for that slot (env pinning is frozen while the op is
  -- outstanding, below, so the reservation still holds at completion).
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

  -- SPMCP_install over a live X/XWB slot has no SLICC transition (panic);
  -- the software slot-lifecycle contract releases before reclaiming.
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

---------------------------------------------------------------------------
-- CPU op delivery to L0/L1 (mandatory queue; parked ops = disabled guards)
---------------------------------------------------------------------------

ruleset c: CoreT do

  -- L0 hit: load completes core-side without touching the L1
  rule "L0 load hit"
    cpu_op[c] = OP_LD & !cpu_busy[c] & l0_present[c] & in_l0_cache(l1s[c])
  ==>
  begin
    if !wb_race then
      assert l0_data[c] = latest "L0 load hit returned a stale value";
    endif;
    cpu_done(c);
  endrule;

  -- L0 hit store: silent E->M upgrade inside L0; L1 state unchanged
  rule "L0 store hit"
    cpu_op[c] = OP_ST & !cpu_busy[c] & l0_present[c] &
    (l1s[c] = L1_E | l1s[c] = L1_M)
  ==>
  begin
    apply_store(c);
  endrule;

  -- Load delivered to L1 (L0 miss path)
  -- A coherent fetch needs a free physical way for the incoming line.  Under
  -- the >= MIN_FREE contract there is always one; without it the fetch stalls
  -- until an SPM slot is released (SPM slots are pinned, not evictable).
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
    -- h_data_to_l0: grant clean copy to L0
    l1s[c] := L1_S;
    l0_present[c] := true; l0_dirty[c] := false; l0_data[c] := l1_data[c];
    if !wb_race then
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
    if !wb_race then
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
    if !wb_race then
      assert l1_data[c] = latest "modified load hit returned a stale value";
    endif;
    cpu_done(c);
  endrule;

  -- Store delivered to L1
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

  -- SPMCP_fetch delivered to L1 (source line state decides the path)
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

  rule "L1 SPMCP_fetch S -> SX_L0 (recall L0)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] & l1s[c] = L1_S & l0q_cnt[c] = 0
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_SX_L0;
    send_recall(c);
  endrule;

  rule "L1 SPMCP_fetch E -> EX_L0 (recall L0)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] & l1s[c] = L1_E & l0q_cnt[c] = 0
  ==>
  begin
    cpu_busy[c] := true;
    l1s[c] := L1_EX_L0;
    send_recall(c);
  endrule;

  rule "L1 SPMCP_fetch M -> MX_L0 (recall L0)"
    cpu_op[c] = OP_CPF & !cpu_busy[c] & l1s[c] = L1_M & l0q_cnt[c] = 0
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
    spm_install(c, l1_data[c]);
    cpu_done(c);
  endrule;

  -- slot-lifecycle ops (slot address; SLICC state I / X / XWB).
  --
  -- Mode A: the SPM slot lands on a FREE way, no coherent occupant displaced.
  -- The >= MIN_FREE contract (spm_ways stays <= WAYS-MIN_FREE) guarantees a
  -- free way exists both for the slot and for later coherent fetches.
  rule "L1 SPMCP_install (free way, no displacement)"
    cpu_op[c] = OP_INST & !cpu_busy[c] & spm_st[c] = SP_INV & l0q_cnt[c] = 0 &
    spm_ways(c) + 1 <= WAYS - MIN_FREE & free_ways(c) >= 1
  ==>
  begin
    spm_st[c] := SP_X;
    spm_data[c] := 0;          -- DataBlk.clear()
    spm_expected[c] := 0;
    cpu_done(c);
  endrule;

  -- Mode B: the SPM slot address collides with the way holding the coherent
  -- line, so that line is LAZILY MIGRATED to a free way (pure local placement:
  -- l1s / l1_data / directory role all preserved -- the directory is way-
  -- agnostic, so this is coherence-neutral) before the slot is installed.
  --
  -- Two obligations the unified model checks that the split models could not:
  --   * the displaced occupant must be STABLE coherent (SLICC allocateSPMSlot
  --     asserts IsStableCoherent).  Displacing a line mid-coherence-transaction
  --     is FINDING F9; ASSUME_FIXES models the software contract that install
  --     targets never alias a transient line's way.
  --   * the migration needs a free way for the moved line.  Because the line
  --     ITSELF occupies one non-SPM way, free = WAYS - spm_ways - 1, so the
  --     contract must reserve >= 2 non-SPM ways (MIN_FREE=2) to guarantee a
  --     home -- one more than the standalone placement model needed.  If none
  --     exists this is the no-free-way panic (FINDING, guarded by MIN_FREE).
  rule "L1 SPMCP_install (displaces + migrates coherent line)"
    cpu_op[c] = OP_INST & !cpu_busy[c] & spm_st[c] = SP_INV & l0q_cnt[c] = 0 &
    line_resident(c) &
    spm_ways(c) + 1 <= WAYS - MIN_FREE &           -- contract cap (as mode A)
    (INSTALL_STABLE_ONLY = 0 | line_stable(c))     -- F9: else displace transient
  ==>
  begin
    if !line_stable(c) then
      -- F9 (only reachable with ASSUME_FIXES=0): install would displace a
      -- coherence-transient line -> gem5 IsStableCoherent assert -> panic.
      error "SPMCP_install displaced a coherence-transient coherent line (IsStableCoherent)";
    elsif free_ways(c) >= 1 then
      -- lazy migration: the line moves to a free way, coherence-neutral.
      -- l1s[c] and l1_data[c] are intentionally UNCHANGED (way is invisible to
      -- the directory); only its physical placement changes.
      saw_migrate[c] := true;
      spm_st[c] := SP_X;
      spm_data[c] := 0;
      spm_expected[c] := 0;
      cpu_done(c);
    else
      -- no free way for the displaced line: the Ruby prototype panics here.
      no_free_way := true;
      cpu_done(c);
    endif;
  endrule;

  -- Environment: other live SPM slots in the same set are pinned/released,
  -- exercising the way-capacity contract.  Pinning respects the contract
  -- (spm_ways stays <= WAYS-MIN_FREE); release is always available so a way
  -- can free up for a stalled coherent fetch (keeps liveness intact).
  rule "env pin extra SPM slot"
    pinned[c] < MAXPIN & spm_ways(c) + 1 <= WAYS - MIN_FREE & free_ways(c) >= 1 &
    -- freeze pinning while this core has an outstanding op that will claim the
    -- primary slot, so the way reserved for it at issue is still there at
    -- completion (keeps spm_ways within the contract cap)
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
    -- absent slot: readSPMData on invalid slot returns zero; either way ack
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
    -- non-X slot: SLICC acks without touching anything
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

---------------------------------------------------------------------------
-- abstract L0 environment: recall responses and spontaneous evictions
---------------------------------------------------------------------------

ruleset c: CoreT do

  -- L0 answers an outstanding INV_OWN/INV_ELSE recall
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

  -- L0 silently drops a clean shared copy (S Replacement row)
  rule "L0 silent clean S drop"
    l0_present[c] & !l0_dirty[c] &
    (l1s[c] = L1_S | l1s[c] = L1_S_IL0 | l1s[c] = L1_SX_L0)
  ==>
  begin
    l0_present[c] := false;
    l0_data[c] := 0;
  endrule;

  -- L0 evicts an E/M copy: PUTX (WriteBack) to L1, possibly with dirty data.
  -- This may race an in-flight recall; the ordered queue keeps WB first.
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

---------------------------------------------------------------------------
-- L1 consumes the ordered L0->L1 queue
---------------------------------------------------------------------------

ruleset c: CoreT do

  rule "L1 handles L0 queue head"
    l0q_cnt[c] > 0
  ==>
  var k: L0MsgKindT; d: boolean; v: ValT;
  begin
    k := l0q[c][0].kind; d := l0q[c][0].dirty; v := l0q[c][0].val;
    l0q_pop(c);

    if k = L0M_WB then
      -- Event WriteBack
      if l1s[c] = L1_M | l1s[c] = L1_E then
        if d then l1_data[c] := v; l1_dirty[c] := true; endif;
        l1s[c] := L1_MM;
      elsif l1s[c] = L1_M_IL0 | l1s[c] = L1_E_IL0 then
        if d then l1_data[c] := v; l1_dirty[c] := true; endif;
        l1s[c] := L1_MM_IL0;
      elsif ASSUME_FIXES = 1 &
            (l1s[c] = L1_MX_L0 | l1s[c] = L1_EX_L0) then
        -- FINDING F2 (confirmed reachable): an L0 capacity eviction (PUTX)
        -- racing the SPMCP_fetch L0-recall delivers WriteBack in MX_L0/EX_L0,
        -- which the SLICC does not define -> panic.  Fix = absorb the data
        -- and keep waiting for the recall response (an INV_ACK from the now
        -- absent L0), mirroring {M,E} WriteBack -> MM.
        if d then l1_data[c] := v; l1_dirty[c] := true; endif;
      else
        -- includes SX_L0/EX_L0/MX_L0: SLICC has no transition -> panic
        error "L1: WriteBack from L0 in a state with no SLICC transition";
      endif;

    elsif k = L0M_IACK then
      -- Event L0_Ack
      if l1s[c] = L1_S_IL0 then l1s[c] := L1_SS;
      elsif l1s[c] = L1_E_IL0 then l1s[c] := L1_EE;
      elsif l1s[c] = L1_M_IL0 then l1s[c] := L1_MM;
      elsif l1s[c] = L1_MM_IL0 then l1s[c] := L1_MM;
      elsif l1s[c] = L1_SM_IL0 then l1s[c] := L1_IM;
      elsif l1s[c] = L1_SX_L0 then
        l1s[c] := L1_SS; spm_install(c, l1_data[c]); cpu_done(c);
      elsif l1s[c] = L1_EX_L0 then
        -- FINDING F7 (see L0_DataAck branch): the coherent source must rest
        -- in MM if it now carries dirty data (from a WriteBack absorbed while
        -- in EX_L0, or a prior L0 store), else EE.  The SLICC EX_L0+L0_Ack
        -- unconditionally chooses EE, which loses the store on a later Inv.
        if ASSUME_FIXES = 1 & l1_dirty[c] then
          l1s[c] := L1_MM;
        else
          l1s[c] := L1_EE;
        endif;
        spm_install(c, l1_data[c]); cpu_done(c);
      elsif l1s[c] = L1_MX_L0 then
        l1s[c] := L1_MM; spm_install(c, l1_data[c]); cpu_done(c);
      else
        error "L1: L0_Ack in a state with no SLICC transition";
      endif;

    else
      -- Event L0_DataAck (dirty recall data)
      if l1s[c] = L1_M_IL0 | l1s[c] = L1_E_IL0 then
        l1_data[c] := v; l1_dirty[c] := true;
        l1s[c] := L1_MM;
      elsif l1s[c] = L1_SX_L0 then
        l1_data[c] := v; l1_dirty[c] := true;
        l1s[c] := L1_SS; spm_install(c, l1_data[c]); cpu_done(c);
      elsif l1s[c] = L1_EX_L0 then
        l1_data[c] := v; l1_dirty[c] := true;
        -- FINDING F7 (confirmed reachable, NO wb_race -> clean bug): the
        -- SPMCP recall of an Exclusive source that carries an un-written-back
        -- dirty L0 store (E at L1, M at L0) captures the dirty value but the
        -- SLICC transition EX_L0 + L0_DataAck -> EE (L1cache.sm:1506) marks
        -- the coherent source EE-with-Dirty=true instead of MM.  Unlike the
        -- normal recall path E_IL0 + L0_DataAck -> MM (line 1905), a later
        -- L2-driven Inv then hits EE + Inv -> I ("don't send data", line
        -- 1709) and the store is silently lost; L2 writes its stale copy to
        -- memory.  Fix = go to MM (a genuine modified owner that answers Inv
        -- with data), mirroring MX_L0 and the normal E_IL0 recall.
        if ASSUME_FIXES = 1 then
          l1s[c] := L1_MM;
        else
          l1s[c] := L1_EE;
        endif;
        spm_install(c, l1_data[c]); cpu_done(c);
      elsif l1s[c] = L1_MX_L0 then
        l1_data[c] := v; l1_dirty[c] := true;
        l1s[c] := L1_MM; spm_install(c, l1_data[c]); cpu_done(c);
      else
        error "L1: L0_DataAck in a state with no SLICC transition";
      endif;
    endif;
  endrule;

endruleset;

---------------------------------------------------------------------------
-- L1 replacement pressure (environment; SLICC victims under set pressure)
---------------------------------------------------------------------------

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

---------------------------------------------------------------------------
-- L1 consumes forwarded requests from L2 (vnet 2)
---------------------------------------------------------------------------

ruleset c: CoreT; i: FwdIdx do

  -- any forwarded coherent request finding the line in L0 recalls it first
  -- (in_port L0_Invalidate_Else path); the message itself is not consumed
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

  rule "L1 handles Inv"
    fwd[c][i].valid & fwd[c][i].t = FW_INV &
    !in_l0_cache(l1s[c]) &
    -- z2 parking states
    !(l1s[c] = L1_MM_IL0 | l1s[c] = L1_SX_L0 | l1s[c] = L1_MX_L0 |
      l1s[c] = L1_EX_L0) &
    -- FINDING F8 (confirmed reachable DEADLOCK; stale-sharer variant of the
    -- PageRank Fwd_GETS_Silent stampede): IX_D stalls Inv (L1cache.sm:1367),
    -- but the Inv comes from another core's UPGRADE that L2 cannot complete
    -- (SS_MB) until this stale sharer acks, while this core's own
    -- GETS_SILENT is queued behind that same SS_MB block -> circular wait.
    -- Fix: an IX_D core holds NO coherent copy (it is fetching a snapshot
    -- into SPM), so it can InvAck the stale-sharer invalidation immediately
    -- and let the independent SPM fetch proceed.
    (ASSUME_FIXES = 1 | l1s[c] != L1_IX_D)
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
      l1s[c] := L1_I; l1_data[c] := 0; l1_dirty[c] := false;
    elsif l1s[c] = L1_EE then
      if rl2 then push_rsp2(R2_ACK, c, 0, false);
      else push_rspc(rq, RC_ACK, 0, false, 1, true); endif;
      l1s[c] := L1_I; l1_data[c] := 0; l1_dirty[c] := false;
    elsif l1s[c] = L1_MM then
      -- f_sendDataToL2
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
      -- ft_sendDataToL2_fromTBE
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
    fwd[c][i].valid & fwd[c][i].t = FW_SILENT &
    !in_l0_cache(l1s[c]) &
    -- 1409: freshest data not settled yet -> stall
    !(l1s[c] = L1_SX_L0 | l1s[c] = L1_MX_L0 | l1s[c] = L1_EX_L0 |
      l1s[c] = L1_IX_D | l1s[c] = L1_SINK) &
    -- 1939: recall in flight -> stall
    !(l1s[c] = L1_MM_IL0)
  ==>
  var rq: CoreT;
  begin
    rq := fwd[c][i].req;
    clear_fwd(c, i);
    if l1s[c] = L1_MM | l1s[c] = L1_EE then
      -- snapshot + Unblock, coherence state untouched
      push_rspc(rq, RC_DATA, l1_data[c], false, 0, true);
      push_unb(UB_UNB, c);
    elsif l1s[c] = L1_M_I then
      -- FINDING F6 (confirmed reachable; matches the PageRank "sequencer
      -- unanswered" stampede): SLICC has the dying owner serve the snapshot
      -- from its writeback TBE and send a plain Unblock, so L2 returns
      -- MT_SPMS -> MT with a DEAD owner.  The next request forwarded to that
      -- owner hits SINK_WB_ACK: Fwd_GETS/Fwd_GETX panic (no transition) and
      -- Fwd_GETS_Silent deadlocks (owner parks it until WB_Ack, L2 MT_SPMS
      -- parks the PUTX until an Unblock that never comes).
      -- Fix: the M_I owner also hands its TBE data to L2 instead of the
      -- Unblock, and L2 takes MT_SPMS + WB_Data -> M (owner has left).
      push_rspc(rq, RC_DATA, l1t_data[c], false, 0, true);
      if ASSUME_FIXES = 1 then
        push_rsp2(R2_WBDATA, c, l1t_data[c], l1t_dirty[c]);
      else
        push_unb(UB_UNB, c);
      endif;
      l1s[c] := L1_SINK;
    else
      -- I/S-family/SS/IS/IM/SM/IS_I have no Fwd_GETS_Silent transition
      error "L1: Fwd_GETS_Silent in a state with no SLICC transition";
    endif;
  endrule;

endruleset;

---------------------------------------------------------------------------
-- L1 consumes responses (vnet 1); never stalled in SLICC
---------------------------------------------------------------------------

ruleset c: CoreT; i: RspCIdx do

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
      if fl1 then push_unb(UB_UNB, c); endif;   -- DataS_fromL1
      if !fl1 & a != 0 then
        error "L1: IS data with outstanding acks is not a SLICC path";
      endif;
      l1s[c] := L1_S;
      l0_present[c] := true; l0_dirty[c] := false; l0_data[c] := v;
      if !wb_race then
        assert v = latest "load miss returned a stale value";
      endif;
      cpu_done(c);
    elsif l1s[c] = L1_IS_I then
      -- stale data handed to L0 once (h_stale_data_to_l0); no freshness claim
      if fl1 then push_unb(UB_UNB, c); endif;
      l1s[c] := L1_I;
      l1_data[c] := 0; l1_dirty[c] := false;
      cpu_done(c);
    elsif l1s[c] = L1_IM then
      if l1t_pend[c] - a = 0 then
        -- Data_all_Acks: store retires
        l1_data[c] := v; l1_dirty[c] := d;
        push_unb(UB_XUNB, c);
        l1s[c] := L1_M;
        apply_store(c);
      else
        -- Data (acks outstanding)
        l1_data[c] := v; l1_dirty[c] := d;
        l1t_pend[c] := l1t_pend[c] - a;
        l1s[c] := L1_SM;
      endif;
    elsif l1s[c] = L1_IX_D then
      -- silent snapshot (or L2/dir data) routed into the SPM slot
      l1s[c] := L1_I;
      spm_install(c, v);
      assert_silent(c);
      cpu_done(c);
    elsif wb_race then
      -- Poisoned line (epoch-contract violation already recorded by F5).
      -- gem5 would actually panic here (DATA delivered to SM, which has no
      -- transition) -- that sequencer crash is itself a consequence of F5.
      -- Drain gracefully so the contract-RESPECTING state space keeps being
      -- explored; correctness claims are already disabled under wb_race.
      l1s[c] := L1_M; l1_data[c] := v; l1_dirty[c] := d;
      push_unb(UB_XUNB, c);
      apply_store(c);
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
      if !wb_race then
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
        -- Ack_all: upgrade retires
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

---------------------------------------------------------------------------
-- L2 consumes L1 requests (vnet 0)
---------------------------------------------------------------------------

ruleset i: ReqIdx do

  rule "L2 handles request"
    req[i].valid &
    -- stall_and_wait states per request type (guards keep msg pooled):
    -- GETS/GETX/UPG/SILENT/SPMWB park in blocked+writeback+transient states
    ( (req[i].t = RQ_PUTX &
        !(l2s = L2_MT_IIB | l2s = L2_MT_SPMS | l2s = L2_MT_MB |
          l2s = L2_SS_MB | l2s = L2_MT_I | l2s = L2_MCT_I) &
        -- FINDING F3 (confirmed reachable): an owner PUTX racing another
        -- core's SPMWB_REQ arrives at L2 in SPM_WB, where L2cache.sm defines
        -- no L1_PUTX/L1_PUTX_old transition (1074/1079 omit SPM_WB and 1446
        -- does not park PUTX) -> panic; fix = stall PUTX until SPMWB_Ack
        !(ASSUME_FIXES = 1 & l2s = L2_SPM_WB))
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
        -- FINDING F1 (confirmed reachable): L2 IM has no transition for
        -- SPM_GETS_SILENT / SPMWB_REQ (L2cache.sm:1200 stalls them only in
        -- {IS, ISS}); fix = stall in IM too
        !( ASSUME_FIXES = 1 & l2s = L2_IM &
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
      n: PendT;
  begin
    c := req[i].src; v := req[i].val; d := req[i].dirty;
    is_upg_sharer := (req[i].t = RQ_UPG) & l2_sharer[c];

    if req[i].t = RQ_PUTX then
      clear_req(i);
      -- L1_PUTX (sharer) vs L1_PUTX_old (stale)
      if l2_sharer[c] then
        if l2s = L2_MT then
          -- owner writes back: MT -> M
          for o: CoreT do l2_sharer[o] := false; endfor;
          if d then l2_data := v; l2_dirty := true; endif;
          push_rspc(c, RC_WBACK, 0, false, 0, false);
          l2s := L2_M;
        elsif l2s = L2_NP | l2s = L2_IS | l2s = L2_ISS | l2s = L2_IM |
              l2s = L2_SS | l2s = L2_M | l2s = L2_M_I | l2s = L2_I_I |
              l2s = L2_S_I | l2s = L2_MT_IB | l2s = L2_MT_SB |
              (ASSUME_FIXES = 1 & l2s = L2_SPM_IS) then
          -- FINDING F4 (confirmed reachable): stale PUTX arriving in SPM_IS
          -- (silent fill in flight) has no SLICC transition; fix = WB_ACK it
          -- exactly like the IS/ISS fill transients
          push_rspc(c, RC_WBACK, 0, false, 0, false);
        else
          -- includes SPM_IS / SPM_WB: no SLICC transition
          error "L2: L1_PUTX in a state with no SLICC transition";
        endif;
      else
        if l2s = L2_NP | l2s = L2_SS | l2s = L2_M | l2s = L2_MT |
           l2s = L2_M_I | l2s = L2_I_I | l2s = L2_S_I | l2s = L2_IS |
           l2s = L2_ISS | l2s = L2_IM | l2s = L2_MT_IB | l2s = L2_MT_SB |
           (ASSUME_FIXES = 1 & l2s = L2_SPM_IS) then
          -- FINDING F4: see above
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
        -- d_sendDataToRequestor + INV to sharers minus requestor
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
      -- L1_UPGRADE (requestor is a sharer)
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
        push_dreq(DQ_SILENT, 0);
        l2s := L2_SPM_IS;
      elsif l2s = L2_SS then
        -- snapshot, no sharer registration
        push_rspc(c, RC_DATA, l2_data, false, 0, false);
      elsif l2s = L2_M then
        push_rspc(c, RC_DATA, l2_data, false, 0, false);
      elsif l2s = L2_MT then
        push_fwd(l2_excl, FW_SILENT, false, c);
        l2s := L2_MT_SPMS;
      else
        error "L2: SPM_GETS_SILENT in a state with no SLICC transition";
      endif;

    else
      -- RQ_SPMWB.  This block models the PRE-FIX F5 bug (spc_clearSPMOwner
      -- wipes directory metadata without invalidating L1 copies -> wb_race).
      -- The IMPLEMENTED fix (MESI_Three_Level_SPM-L2cache.sm, new SPM_WB_INV
      -- state) invalidates the coherent sharers/owner and collects their acks
      -- BEFORE overwriting, so no reader is left stale.  That async-invalidation
      -- fix is validated end-to-end in gem5 (spm_reuse 4-core checksum matches
      -- the coherent-cache reference), not re-modeled here; this path is kept
      -- as the documented characterization of the original defect.
      clear_req(i);
      if l2s = L2_NP | l2s = L2_SS | l2s = L2_M | l2s = L2_MT then
        -- architecturally a write to the line: flag software-contract races
        if !line_quiet_for_wb() then
          wb_race := true;
        endif;
        for o: CoreT do l2_sharer[o] := false; endfor;  -- spc_clearSPMOwner
        l2_data := v;
        l2_dirty := true;
        if !wb_race then
          ghost_store(v);
        endif;
        l2_clear_tbe();
        l2t_req := c;
        push_dreq(DQ_SPMWB, v);
        l2s := L2_SPM_WB;
      else
        error "L2: SPMWB_REQ in a state with no SLICC transition";
      endif;
    endif;
  endrule;

endruleset;

---------------------------------------------------------------------------
-- L2 consumes responses (vnet 1) and dir INVs
---------------------------------------------------------------------------

ruleset i: Rsp2Idx do

  rule "L2 handles response"
    rsp2[i].valid
  ==>
  var t: Rsp2TypeT; c: CoreT; v: ValT; d: boolean;
  begin
    t := rsp2[i].t; c := rsp2[i].src; v := rsp2[i].val; d := rsp2[i].dirty;
    clear_rsp2(i);

    if t = R2_WBDATA then
      -- WB_Data (dirty) / WB_Data_clean
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
          push_d2d(DD_DATA, l2t_data, l2t_dirty);  -- ct_...FromTBE
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
      elsif ASSUME_FIXES = 1 & l2s = L2_MT_SPMS then
        -- FINDING F6 fix, L2 side: the silently-snapshotted owner was dying
        -- (M_I) and handed the line back; absorb it and drop to M
        l2_data := v;
        if d then l2_dirty := true; endif;
        for o: CoreT do l2_sharer[o] := false; endfor;
        l2s := L2_M;
      else
        error "L2: WB_Data in a state with no SLICC transition";
      endif;

    elsif t = R2_ACK then
      -- inv acks aimed at the L2 (replacement flows)
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
        -- silent fill: hand snapshot straight to the requester, drop block
        push_rspc(l2t_req, RC_DATA, v, false, 0, false);
        l2_dealloc();
        l2_clear_tbe();
        l2s := L2_NP;
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
      -- R2_SPMWBACK
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

-- MEM_Inv from the directory (recycled while L2 transient)
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
  -- NP / *_I writeback states: sink it
endrule;

---------------------------------------------------------------------------
-- L2 consumes unblocks (vnet 2)
---------------------------------------------------------------------------

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
        -- silent snapshot completed by the owner; requester NOT a sharer
        l2s := L2_MT;
      else
        error "L2: Unblock in a state with no SLICC transition";
      endif;
    endif;
  endrule;

endruleset;

---------------------------------------------------------------------------
-- L2 replacement pressure (environment)
---------------------------------------------------------------------------

rule "L2 replacement SS"
  l2s = L2_SS
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
  l2s = L2_M
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

---------------------------------------------------------------------------
-- memory directory
---------------------------------------------------------------------------

ruleset i: DReqIdx do

  -- dir M holding a parked fetch sends one INV to the (stale) L2 owner
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
      -- CleanReplacement
      push_rsp2(R2_MEMACK, 0, 0, false);
      dirs := D_I;
    endif;
  endrule;

  -- DD_DATA parked while dir transient (675); DD_ACK in a transient state
  -- has no SLICC transition
  rule "dir CleanReplacement undefined in transients"
    d2d[i].valid & d2d[i].t = DD_ACK &
    (dirs = D_IM | dirs = D_MI | dirs = D_SPM_IS | dirs = D_SPM_WB)
  ==>
  begin
    error "dir: CleanReplacement in a state with no SLICC transition";
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
    else
      error "dir: Memory_Ack in a state with no SLICC transition";
    endif;
  endif;
endrule;

---------------------------------------------------------------------------
-- invariants
---------------------------------------------------------------------------

-- single-writer / multiple-reader over stable + recall states.
-- FINDING F5 (confirmed reachable): guarded by wb_race because SPMWB_REQ in
-- L2 SS/MT clears sharer/owner tracking (spc_clearSPMOwner) WITHOUT
-- invalidating the actual L1 copies; L2 then reaches M and can grant a
-- second Exclusive copy while the old owner still holds E/M -> genuine SWMR
-- violation (silent corruption, no panic).  The protocol does not defend
-- itself against an epoch-contract violation; SLICC should either stall
-- SPMWB_REQ while sharers/owner exist or invalidate them first.
invariant "SWMR"
  wb_race |
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

-- every readable copy carries the latest coherent value
invariant "shared copies fresh"
  wb_race |
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
  wb_race | (l2s = L2_SS -> l2_data = latest);

invariant "L2 exclusive-free data fresh"
  wb_race | (l2s = L2_M -> l2_data = latest);

-- live SPM slots are isolated from the coherence domain
invariant "SPM slot isolation"
  forall c: CoreT do
    (spm_st[c] = SP_X | spm_st[c] = SP_XWB) -> spm_data[c] = spm_expected[c]
  endforall;

-- UNIFIED placement property: under the >= MIN_FREE contract, SPMCP_install's
-- lazy migration of a displaced coherent line always has a free way.
invariant "install migration never hits the no-free-way panic"
  no_free_way -> false;

-- physical way accounting stays within the associativity (the line + all SPM
-- ways never exceed WAYS)
invariant "L1 set never oversubscribed"
  forall c: CoreT do
    (line_resident(c) -> spm_ways(c) + 1 <= WAYS) &
    (!line_resident(c) -> spm_ways(c) <= WAYS)
  endforall;

-- the contract holds: at least MIN_FREE non-SPM ways are always reserved
invariant "contract keeps MIN_FREE non-SPM ways"
  forall c: CoreT do
    spm_ways(c) <= WAYS - MIN_FREE
  endforall;

-- an SPM slot never doubles as a directory-visible coherent copy
invariant "L2 MT has a plausible owner"
  wb_race |
  (l2s = L2_MT ->
    (l2_excl != NCORE &
     !(l1s[l2_excl] = L1_S | l1s[l2_excl] = L1_SS |
       l1s[l2_excl] = L1_S_IL0 | l1s[l2_excl] = L1_SX_L0)));

invariant "L2 stable-shared implies no exclusive L1"
  wb_race |
  (l2s = L2_SS ->
    forall c: CoreT do
      !(l1s[c] = L1_E | l1s[c] = L1_EE | l1s[c] = L1_M | l1s[c] = L1_MM |
        l1s[c] = L1_E_IL0 | l1s[c] = L1_M_IL0 | l1s[c] = L1_MM_IL0 |
        l1s[c] = L1_EX_L0 | l1s[c] = L1_MX_L0)
    endforall);

-- quiescent memory freshness: with no cached copies and no traffic,
-- memory holds the latest value
invariant "quiescent memory fresh"
  wb_race |
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

---------------------------------------------------------------------------
-- liveness: no core can be wedged forever (the stampede-deadlock property)
---------------------------------------------------------------------------

liveness "all cores can drain"
  forall c: CoreT do cpu_op[c] = OP_NONE endforall;
