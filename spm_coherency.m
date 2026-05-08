-- Rumur/Murphi model for the SPM coherence state tables in:
--   spm_coherency_cc.csv
--   spm_coherency_dir.csv
--
-- Scope:
--   * one cache line
--   * three cache controllers
--   * one directory
--   * unordered finite network with nondeterministic delivery
--
-- This is intentionally an explicit protocol model, not an atomic transaction
-- summary. The CC transient states, directory SD state, forwarded requests,
-- invalidations, put acknowledgements, owner data responses, and ack counting
-- are modeled as separate steps.

const
  NODE_COUNT: 3;
  NET_MAX: 24;

type
  Node: scalarset(NODE_COUNT);
  MsgSlot: 0..NET_MAX;
  AckCount: 0..NODE_COUNT;

  CCState: enum {
    CC_I,
    CC_IS_D,
    CC_IM_AD,
    CC_IM_A,
    CC_S,
    CC_SX_L,
    CC_SX_A,
    CC_SM_AD,
    CC_SM_A,
    CC_M,
    CC_MX_L,
    CC_MX_A,
    CC_E,
    CC_EX_L,
    CC_EX_A,
    CC_MI_A,
    CC_EI_A,
    CC_SI_A,
    CC_II_A,
    CC_II_D,
    CC_X
  };

  DirState: enum {
    DIR_I,
    DIR_S,
    DIR_E,
    DIR_M,
    DIR_SD
  };

  MsgType: enum {
    MSG_GetS,
    MSG_GetS_silent,
    MSG_GetM,
    MSG_PutS,
    MSG_PutM,
    MSG_PutE,
    MSG_FwdGetS,
    MSG_FwdGetM,
    MSG_Inv,
    MSG_InvAck,
    MSG_PutAck,
    MSG_ExclDataDir,
    MSG_DataDir,
    MSG_DataOwner,
    MSG_DataToDir
  };

  Message: record
    mtype: MsgType;
    src: Node;
    dst: Node;
    req: Node;
    to_dir: boolean;
    silent: boolean;
    ack: AckCount;
  end;

var
  cc: array [Node] of CCState;
  cc_ack: array [Node] of AckCount;

  dir: DirState;
  owner_valid: boolean;
  owner: Node;
  sharer: array [Node] of boolean;

  mshr_valid: boolean;
  mshr_req: Node;
  mshr_owner: Node;
  mshr_silent: boolean;

  net_valid: array [MsgSlot] of boolean;
  net: array [MsgSlot] of Message;

function StableCoherent(s: CCState): boolean;
begin
  return s = CC_S | s = CC_E | s = CC_M;
end;

function StableReadable(s: CCState): boolean;
begin
  return s = CC_S | s = CC_E | s = CC_M;
end;

function StableWritable(s: CCState): boolean;
begin
  return s = CC_E | s = CC_M;
end;

function WaitingForData(s: CCState): boolean;
begin
  return s = CC_IS_D | s = CC_IM_AD | s = CC_SM_AD | s = CC_II_D;
end;

function WaitingForAck(s: CCState): boolean;
begin
  return
    s = CC_IM_A | s = CC_SM_A |
    s = CC_SI_A | s = CC_MI_A | s = CC_EI_A |
    s = CC_II_A | s = CC_SX_A | s = CC_MX_A | s = CC_EX_A;
end;

function MigratingCoherent(s: CCState): boolean;
begin
  return s = CC_SX_L | s = CC_MX_L | s = CC_EX_L;
end;

function CountSharers(): AckCount;
var
  n: Node;
  count: AckCount;
begin
  count := 0;
  for n: Node do
    if sharer[n] then
      count := count + 1;
    endif;
  endfor;
  return count;
end;

function CountInvTargets(req: Node): AckCount;
var
  n: Node;
  count: AckCount;
begin
  count := 0;
  for n: Node do
    if sharer[n] & n != req then
      count := count + 1;
    endif;
  endfor;
  return count;
end;

function InvTargetsReady(req: Node): boolean;
var
  n: Node;
begin
  for n: Node do
    if sharer[n] & n != req then
      if !(cc[n] = CC_S | cc[n] = CC_SX_L | cc[n] = CC_SX_A |
           cc[n] = CC_SM_AD | cc[n] = CC_SI_A) then
        return false;
      endif;
    endif;
  endfor;
  return true;
end;

function FreeSlots(): 0..NET_MAX + 1;
var
  i: MsgSlot;
  count: 0..NET_MAX + 1;
begin
  count := 0;
  for i: MsgSlot do
    if !net_valid[i] then
      count := count + 1;
    endif;
  endfor;
  return count;
end;

function HasMsg(t: MsgType; n: Node): boolean;
var
  i: MsgSlot;
begin
  for i: MsgSlot do
    if net_valid[i] & !net[i].to_dir & net[i].dst = n & net[i].mtype = t then
      return true;
    endif;
  endfor;
  return false;
end;

function HasDirMsg(t: MsgType): boolean;
var
  i: MsgSlot;
begin
  for i: MsgSlot do
    if net_valid[i] & net[i].to_dir & net[i].mtype = t then
      return true;
    endif;
  endfor;
  return false;
end;

function HasPendingFwd(n: Node): boolean;
begin
  return HasMsg(MSG_FwdGetS, n) | HasMsg(MSG_FwdGetM, n);
end;

function HasPendingInv(n: Node): boolean;
begin
  return HasMsg(MSG_Inv, n);
end;

function HasPendingData(n: Node): boolean;
begin
  return HasMsg(MSG_DataDir, n) | HasMsg(MSG_ExclDataDir, n) | HasMsg(MSG_DataOwner, n);
end;

function HasDataToDirFor(n: Node): boolean;
var
  i: MsgSlot;
begin
  for i: MsgSlot do
    if net_valid[i] & net[i].to_dir & net[i].mtype = MSG_DataToDir & net[i].req = n then
      return true;
    endif;
  endfor;
  return false;
end;

function HasPendingDirCompletion(n: Node): boolean;
begin
  return (dir = DIR_SD & mshr_valid & mshr_req = n) | HasDataToDirFor(n);
end;

procedure ClearSharers();
var
  n: Node;
begin
  for n: Node do
    sharer[n] := false;
  endfor;
end;

procedure EnqueueToDir(t: MsgType; src: Node; req: Node; silent: boolean; ack: AckCount);
var
  i: MsgSlot;
  done: boolean;
begin
  assert FreeSlots() > 0 "network full";
  done := false;
  for i: MsgSlot do
    if !done & !net_valid[i] then
      net_valid[i] := true;
      net[i].mtype := t;
      net[i].src := src;
      net[i].dst := src;
      net[i].req := req;
      net[i].to_dir := true;
      net[i].silent := silent;
      net[i].ack := ack;
      done := true;
    endif;
  endfor;
end;

procedure EnqueueToNode(t: MsgType; src: Node; dst: Node; req: Node; silent: boolean; ack: AckCount);
var
  i: MsgSlot;
  done: boolean;
begin
  assert FreeSlots() > 0 "network full";
  done := false;
  for i: MsgSlot do
    if !done & !net_valid[i] then
      net_valid[i] := true;
      net[i].mtype := t;
      net[i].src := src;
      net[i].dst := dst;
      net[i].req := req;
      net[i].to_dir := false;
      net[i].silent := silent;
      net[i].ack := ack;
      done := true;
    endif;
  endfor;
end;

procedure RemoveSharer(n: Node);
begin
  sharer[n] := false;
  if dir = DIR_S & CountSharers() = 0 then
    dir := DIR_I;
    owner_valid := false;
  endif;
end;

procedure SendInvs(req: Node);
var
  n: Node;
begin
  for n: Node do
    if sharer[n] & n != req then
      EnqueueToNode(MSG_Inv, req, n, req, false, 0);
    endif;
  endfor;
end;

procedure FinishPutAck(n: Node);
begin
  if cc[n] = CC_SX_A | cc[n] = CC_MX_A | cc[n] = CC_EX_A then
    cc[n] := CC_X;
  else
    cc[n] := CC_I;
  endif;
  cc_ack[n] := 0;
end;

procedure ReceiveInvAck(n: Node);
begin
  if cc[n] = CC_IM_AD | cc[n] = CC_SM_AD then
    if cc_ack[n] < NODE_COUNT then
      cc_ack[n] := cc_ack[n] + 1;
    endif;
  elsif cc_ack[n] > 1 then
    cc_ack[n] := cc_ack[n] - 1;
  else
    cc_ack[n] := 0;
    if cc[n] = CC_IM_AD then
      cc[n] := CC_IM_A;
    elsif cc[n] = CC_SM_AD then
      cc[n] := CC_SM_A;
    elsif cc[n] = CC_IM_A | cc[n] = CC_SM_A then
      cc[n] := CC_M;
    endif;
  endif;
end;

procedure ReceiveSharedData(n: Node);
begin
  if cc[n] = CC_IS_D then
    cc[n] := CC_S;
  elsif cc[n] = CC_II_D then
    cc[n] := CC_I;
  endif;
end;

procedure ReceiveMData(n: Node; ack: AckCount);
begin
  if cc[n] = CC_IM_AD | cc[n] = CC_SM_AD then
    if ack <= cc_ack[n] then
      cc[n] := CC_M;
      cc_ack[n] := 0;
    else
      cc_ack[n] := ack - cc_ack[n];
      if cc[n] = CC_IM_AD then
        cc[n] := CC_IM_A;
      else
        cc[n] := CC_SM_A;
      endif;
    endif;
  endif;
end;

startstate "initially invalid"
var
  n: Node;
  i: MsgSlot;
begin
  for n: Node do
    cc[n] := CC_I;
    cc_ack[n] := 0;
    sharer[n] := false;
  endfor;

  dir := DIR_I;
  owner_valid := false;
  mshr_valid := false;

  for i: MsgSlot do
    net_valid[i] := false;
  endfor;
endstartstate;

-- Processor-side request rules.
ruleset n: Node do
  rule "CC Load: I sends GetS"
    cc[n] = CC_I & !HasPendingDirCompletion(n) & !HasPendingData(n) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_GetS, n, n, false, 0);
    cc[n] := CC_IS_D;
  endrule;

  rule "CC SPMCP_fetch: I sends GetS_silent"
    cc[n] = CC_I & !HasPendingDirCompletion(n) & !HasPendingData(n) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_GetS_silent, n, n, true, 0);
    cc[n] := CC_II_D;
  endrule;

  rule "CC Store miss: I sends GetM"
    cc[n] = CC_I & !HasPendingDirCompletion(n) & !HasPendingData(n) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_GetM, n, n, false, 0);
    cc[n] := CC_IM_AD;
    cc_ack[n] := 0;
  endrule;

  rule "CC Store upgrade: S sends GetM"
    cc[n] = CC_S & !HasPendingDirCompletion(n) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_GetM, n, n, false, 0);
    cc[n] := CC_SM_AD;
    cc_ack[n] := 0;
  endrule;

  rule "CC Store hit: E becomes M"
    cc[n] = CC_E
  ==>
  begin
    cc[n] := CC_M;
    if owner_valid & owner = n then
      dir := DIR_M;
    endif;
  endrule;

  rule "CC SPMCP_install: I installs SPM"
    cc[n] = CC_I & !HasPendingDirCompletion(n) & !HasPendingData(n)
  ==>
  begin
    cc[n] := CC_X;
  endrule;

  rule "CC SPMCP_install: S starts lazy migration"
    cc[n] = CC_S & !HasPendingFwd(n) & !HasPendingInv(n) & !HasPendingDirCompletion(n)
  ==>
  begin
    cc[n] := CC_SX_L;
  endrule;

  rule "CC SPMCP_install: M starts lazy migration"
    cc[n] = CC_M & !HasPendingFwd(n) & !HasPendingInv(n) & !HasPendingDirCompletion(n)
  ==>
  begin
    cc[n] := CC_MX_L;
  endrule;

  rule "CC SPMCP_install: E starts lazy migration"
    cc[n] = CC_E & !HasPendingFwd(n) & !HasPendingInv(n) & !HasPendingDirCompletion(n)
  ==>
  begin
    cc[n] := CC_EX_L;
  endrule;

  rule "CC lazy SPM migration succeeds"
    (cc[n] = CC_SX_L | cc[n] = CC_MX_L | cc[n] = CC_EX_L) &
    !HasPendingFwd(n) & !HasPendingInv(n) & !HasPendingDirCompletion(n)
  ==>
  begin
    -- A successful local migration leaves the coherent copy in an eligible
    -- non-SPM way. For this one-line abstraction we only track the installed
    -- SPM copy, matching the table's visible transition to X.
    cc[n] := CC_X;
    if owner_valid & owner = n then
      owner_valid := false;
      dir := DIR_I;
    endif;
    sharer[n] := false;
  endrule;

  rule "CC lazy S migration fails: send PutS"
    cc[n] = CC_SX_L & !HasPendingFwd(n) & !HasPendingInv(n) & !HasPendingDirCompletion(n) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_PutS, n, n, false, 0);
    cc[n] := CC_SX_A;
  endrule;

  rule "CC lazy M migration fails: send PutM"
    cc[n] = CC_MX_L & !HasPendingFwd(n) & !HasPendingInv(n) & !HasPendingDirCompletion(n) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_PutM, n, n, false, 0);
    cc[n] := CC_MX_A;
  endrule;

  rule "CC lazy E migration fails: send PutE"
    cc[n] = CC_EX_L & !HasPendingFwd(n) & !HasPendingInv(n) & !HasPendingDirCompletion(n) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_PutE, n, n, false, 0);
    cc[n] := CC_EX_A;
  endrule;

  rule "CC Replacement: S sends PutS"
    cc[n] = CC_S & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_PutS, n, n, false, 0);
    cc[n] := CC_SI_A;
  endrule;

  rule "CC Replacement: M sends PutM"
    cc[n] = CC_M & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_PutM, n, n, false, 0);
    cc[n] := CC_MI_A;
  endrule;

  rule "CC Replacement: E sends PutE"
    cc[n] = CC_E & FreeSlots() >= 1
  ==>
  begin
    EnqueueToDir(MSG_PutE, n, n, false, 0);
    cc[n] := CC_EI_A;
  endrule;

  rule "CC SPM_release"
    cc[n] = CC_X & !HasPendingData(n)
  ==>
  begin
    cc[n] := CC_I;
  endrule;
endruleset;

-- Directory receives requests and writebacks.
ruleset i: MsgSlot do
  rule "DIR I handles GetS_silent"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_GetS_silent &
    dir = DIR_I & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_DataDir, net[i].src, net[i].src, net[i].src, true, 0);
    net_valid[i] := false;
  endrule;

  rule "DIR I handles GetS"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_GetS &
    dir = DIR_I & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_ExclDataDir, net[i].src, net[i].src, net[i].src, false, 0);
    dir := DIR_E;
    owner := net[i].src;
    owner_valid := true;
    net_valid[i] := false;
  endrule;

  rule "DIR I handles GetM"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_GetM &
    dir = DIR_I & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_DataDir, net[i].src, net[i].src, net[i].src, false, 0);
    dir := DIR_M;
    owner := net[i].src;
    owner_valid := true;
    net_valid[i] := false;
  endrule;

  rule "DIR S handles GetS_silent"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_GetS_silent &
    dir = DIR_S & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_DataDir, net[i].src, net[i].src, net[i].src, true, 0);
    net_valid[i] := false;
  endrule;

  rule "DIR S handles GetS"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_GetS &
    dir = DIR_S & FreeSlots() >= 1
  ==>
  begin
    sharer[net[i].src] := true;
    EnqueueToNode(MSG_DataDir, net[i].src, net[i].src, net[i].src, false, 0);
    net_valid[i] := false;
  endrule;

  rule "DIR S handles GetM"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_GetM &
    dir = DIR_S & InvTargetsReady(net[i].src) &
    FreeSlots() >= CountInvTargets(net[i].src) + 1
  ==>
  begin
    EnqueueToNode(MSG_DataDir, net[i].src, net[i].src, net[i].src, false, CountInvTargets(net[i].src));
    SendInvs(net[i].src);
    ClearSharers();
    dir := DIR_M;
    owner := net[i].src;
    owner_valid := true;
    net_valid[i] := false;
  endrule;

  rule "DIR E/M handles GetS"
    net_valid[i] & net[i].to_dir &
    (net[i].mtype = MSG_GetS | net[i].mtype = MSG_GetS_silent) &
    (dir = DIR_E | dir = DIR_M) & owner_valid &
    StableCoherent(cc[owner]) &
    !MigratingCoherent(cc[owner]) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_FwdGetS, net[i].src, owner, net[i].src, net[i].mtype = MSG_GetS_silent, 0);
    sharer[owner] := true;
    if net[i].mtype = MSG_GetS then
      sharer[net[i].src] := true;
    endif;
    owner_valid := false;
    mshr_valid := true;
    mshr_req := net[i].src;
    mshr_owner := owner;
    mshr_silent := net[i].mtype = MSG_GetS_silent;
    dir := DIR_SD;
    net_valid[i] := false;
  endrule;

  rule "DIR E/M handles GetM"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_GetM &
    (dir = DIR_E | dir = DIR_M) & owner_valid &
    StableCoherent(cc[owner]) &
    !MigratingCoherent(cc[owner]) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_FwdGetM, net[i].src, owner, net[i].src, false, 0);
    owner := net[i].src;
    owner_valid := true;
    dir := DIR_M;
    ClearSharers();
    net_valid[i] := false;
  endrule;

  rule "DIR handles PutS"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_PutS &
    (dir = DIR_S | dir = DIR_SD | dir = DIR_I | dir = DIR_E | dir = DIR_M) &
    FreeSlots() >= 1
  ==>
  begin
    if dir = DIR_S | dir = DIR_SD then
      RemoveSharer(net[i].src);
    endif;
    EnqueueToNode(MSG_PutAck, net[i].src, net[i].src, net[i].src, false, 0);
    net_valid[i] := false;
  endrule;

  rule "DIR handles PutM"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_PutM &
    FreeSlots() >= 2
  ==>
  begin
    if (dir = DIR_M | dir = DIR_E) & owner_valid & owner = net[i].src then
      owner_valid := false;
      dir := DIR_I;
      ClearSharers();
    elsif dir = DIR_SD & mshr_valid & net[i].src = mshr_owner & net[i].req = mshr_req then
      -- Directory table: data/writeback in SD completes the pending GetS.
      EnqueueToNode(MSG_DataDir, net[i].src, mshr_req, mshr_req, mshr_silent, 0);
      sharer[net[i].src] := false;
      if mshr_silent then
        dir := DIR_I;
      else
        if cc[mshr_req] != CC_SI_A & cc[mshr_req] != CC_II_A & cc[mshr_req] != CC_I then
          sharer[mshr_req] := true;
        endif;
        dir := DIR_S;
      endif;
      owner_valid := false;
      mshr_valid := false;
    elsif dir = DIR_SD then
      RemoveSharer(net[i].src);
    elsif dir = DIR_S then
      RemoveSharer(net[i].src);
    endif;
    EnqueueToNode(MSG_PutAck, net[i].src, net[i].src, net[i].src, false, 0);
    net_valid[i] := false;
  endrule;

  rule "DIR handles PutE"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_PutE &
    FreeSlots() >= 2
  ==>
  begin
    if (dir = DIR_E | dir = DIR_M) & owner_valid & owner = net[i].src then
      owner_valid := false;
      dir := DIR_I;
      ClearSharers();
    elsif dir = DIR_SD & mshr_valid & net[i].src = mshr_owner & net[i].req = mshr_req then
      EnqueueToNode(MSG_DataDir, net[i].src, mshr_req, mshr_req, mshr_silent, 0);
      sharer[net[i].src] := false;
      if mshr_silent then
        dir := DIR_I;
      else
        if cc[mshr_req] != CC_SI_A & cc[mshr_req] != CC_II_A & cc[mshr_req] != CC_I then
          sharer[mshr_req] := true;
        endif;
        dir := DIR_S;
      endif;
      owner_valid := false;
      mshr_valid := false;
    elsif dir = DIR_SD then
      RemoveSharer(net[i].src);
    elsif dir = DIR_S then
      RemoveSharer(net[i].src);
    endif;
    EnqueueToNode(MSG_PutAck, net[i].src, net[i].src, net[i].src, false, 0);
    net_valid[i] := false;
  endrule;

  rule "DIR SD handles owner data"
    net_valid[i] & net[i].to_dir & net[i].mtype = MSG_DataToDir &
    dir = DIR_SD & mshr_valid & net[i].src = mshr_owner & net[i].req = mshr_req
  ==>
  begin
    net_valid[i] := false;
    owner_valid := false;
    if mshr_silent then
      dir := DIR_S;
    else
      if cc[mshr_req] != CC_SI_A & cc[mshr_req] != CC_II_A & cc[mshr_req] != CC_I then
        sharer[mshr_req] := true;
      endif;
      dir := DIR_S;
    endif;
    mshr_valid := false;
  endrule;
endruleset;

-- Cache controllers receive forwarded requests, invalidations, data, and acks.
ruleset i: MsgSlot do
  rule "CC receives Exclusive Data from Dir"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_ExclDataDir &
    cc[net[i].dst] = CC_IS_D
  ==>
  begin
    net_valid[i] := false;
    cc[net[i].dst] := CC_E;
  endrule;

  rule "CC receives Data from Dir for shared/SPM"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_DataDir &
    (cc[net[i].dst] = CC_IS_D | cc[net[i].dst] = CC_II_D)
  ==>
  begin
    net_valid[i] := false;
    ReceiveSharedData(net[i].dst);
  endrule;

  rule "CC receives Data from Dir for GetM"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_DataDir &
    (cc[net[i].dst] = CC_IM_AD | cc[net[i].dst] = CC_SM_AD)
  ==>
  begin
    net_valid[i] := false;
    ReceiveMData(net[i].dst, net[i].ack);
  endrule;

  rule "CC receives Data from Owner"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_DataOwner &
    (cc[net[i].dst] = CC_IS_D | cc[net[i].dst] = CC_II_D |
     cc[net[i].dst] = CC_IM_AD | cc[net[i].dst] = CC_SM_AD)
  ==>
  begin
    net_valid[i] := false;
    if cc[net[i].dst] = CC_IM_AD | cc[net[i].dst] = CC_SM_AD then
      cc[net[i].dst] := CC_M;
      cc_ack[net[i].dst] := 0;
    else
      ReceiveSharedData(net[i].dst);
    endif;
  endrule;

  rule "CC S receives Inv"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_Inv &
    cc[net[i].dst] = CC_S & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_InvAck, net[i].dst, net[i].req, net[i].dst, false, 0);
    cc[net[i].dst] := CC_I;
    net_valid[i] := false;
  endrule;

  rule "CC SX_L receives Inv"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_Inv &
    cc[net[i].dst] = CC_SX_L & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_InvAck, net[i].dst, net[i].req, net[i].dst, false, 0);
    cc[net[i].dst] := CC_X;
    net_valid[i] := false;
  endrule;

  rule "CC SX_A receives Inv"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_Inv &
    cc[net[i].dst] = CC_SX_A & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_InvAck, net[i].dst, net[i].req, net[i].dst, false, 0);
    cc[net[i].dst] := CC_SX_A;
    net_valid[i] := false;
  endrule;

  rule "CC SM_AD receives Inv"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_Inv &
    cc[net[i].dst] = CC_SM_AD & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_InvAck, net[i].dst, net[i].req, net[i].dst, false, 0);
    cc[net[i].dst] := CC_IM_AD;
    net_valid[i] := false;
  endrule;

  rule "CC SI_A receives Inv"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_Inv &
    cc[net[i].dst] = CC_SI_A & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_InvAck, net[i].dst, net[i].req, net[i].dst, false, 0);
    cc[net[i].dst] := CC_II_A;
    net_valid[i] := false;
  endrule;

  rule "CC M/E receives FwdGetS"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_FwdGetS &
    dir = DIR_SD & mshr_valid & mshr_req = net[i].req & mshr_owner = net[i].dst &
    (cc[net[i].dst] = CC_M | cc[net[i].dst] = CC_E) & FreeSlots() >= 2
  ==>
  begin
    EnqueueToNode(MSG_DataOwner, net[i].dst, net[i].req, net[i].req, net[i].silent, 0);
    EnqueueToDir(MSG_DataToDir, net[i].dst, net[i].req, net[i].silent, 0);
    cc[net[i].dst] := CC_S;
    net_valid[i] := false;
  endrule;

  rule "CC M/E receives FwdGetM"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_FwdGetM &
    dir = DIR_M & owner_valid & owner = net[i].req &
    (cc[net[i].dst] = CC_M | cc[net[i].dst] = CC_E) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_DataOwner, net[i].dst, net[i].req, net[i].req, false, 0);
    cc[net[i].dst] := CC_I;
    net_valid[i] := false;
  endrule;

  rule "CC MI/EI receives FwdGetS"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_FwdGetS &
    dir = DIR_SD & mshr_valid & mshr_req = net[i].req & mshr_owner = net[i].dst &
    (cc[net[i].dst] = CC_MI_A | cc[net[i].dst] = CC_EI_A) & FreeSlots() >= 2
  ==>
  begin
    EnqueueToNode(MSG_DataOwner, net[i].dst, net[i].req, net[i].req, net[i].silent, 0);
    EnqueueToDir(MSG_DataToDir, net[i].dst, net[i].req, net[i].silent, 0);
    cc[net[i].dst] := CC_SI_A;
    net_valid[i] := false;
  endrule;

  rule "CC MI/EI receives FwdGetM"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_FwdGetM &
    dir = DIR_M & owner_valid & owner = net[i].req &
    (cc[net[i].dst] = CC_MI_A | cc[net[i].dst] = CC_EI_A) & FreeSlots() >= 1
  ==>
  begin
    EnqueueToNode(MSG_DataOwner, net[i].dst, net[i].req, net[i].req, false, 0);
    cc[net[i].dst] := CC_II_A;
    net_valid[i] := false;
  endrule;

  rule "CC drops stale FwdGetS"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_FwdGetS &
    !(dir = DIR_SD & mshr_valid & mshr_req = net[i].req & mshr_owner = net[i].dst)
  ==>
  begin
    net_valid[i] := false;
  endrule;

  rule "CC drops stale FwdGetM"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_FwdGetM &
    !(dir = DIR_M & owner_valid & owner = net[i].req)
  ==>
  begin
    net_valid[i] := false;
  endrule;

  rule "CC receives Inv-Ack"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_InvAck &
    (cc[net[i].dst] = CC_IM_AD | cc[net[i].dst] = CC_IM_A |
     cc[net[i].dst] = CC_SM_AD | cc[net[i].dst] = CC_SM_A)
  ==>
  begin
    net_valid[i] := false;
    ReceiveInvAck(net[i].dst);
  endrule;

  rule "CC receives Put-Ack"
    net_valid[i] & !net[i].to_dir & net[i].mtype = MSG_PutAck &
    (cc[net[i].dst] = CC_SI_A | cc[net[i].dst] = CC_MI_A |
     cc[net[i].dst] = CC_EI_A | cc[net[i].dst] = CC_II_A |
     cc[net[i].dst] = CC_SX_A | cc[net[i].dst] = CC_MX_A |
     cc[net[i].dst] = CC_EX_A) &
    !HasPendingFwd(net[i].dst) & !HasPendingInv(net[i].dst)
  ==>
  begin
    net_valid[i] := false;
    FinishPutAck(net[i].dst);
  endrule;

  rule "CC drops stale data response"
    net_valid[i] & !net[i].to_dir &
    (net[i].mtype = MSG_DataDir | net[i].mtype = MSG_ExclDataDir |
     net[i].mtype = MSG_DataOwner) &
    !((net[i].mtype = MSG_ExclDataDir & cc[net[i].dst] = CC_IS_D) |
      (net[i].mtype = MSG_DataDir &
       (cc[net[i].dst] = CC_IS_D | cc[net[i].dst] = CC_II_D |
        cc[net[i].dst] = CC_IM_AD | cc[net[i].dst] = CC_SM_AD)) |
      (net[i].mtype = MSG_DataOwner &
       (cc[net[i].dst] = CC_IS_D | cc[net[i].dst] = CC_II_D |
        cc[net[i].dst] = CC_IM_AD | cc[net[i].dst] = CC_SM_AD)))
  ==>
  begin
    net_valid[i] := false;
  endrule;
endruleset;

invariant "at most one stable coherent writer"
  forall a: Node do
    forall b: Node do
      a != b ->
        !(StableWritable(cc[a]) & StableWritable(cc[b]))
    endforall
  endforall;

invariant "stable coherent reader excludes another stable writer"
  forall a: Node do
    forall b: Node do
      a != b ->
        !(StableReadable(cc[a]) & StableWritable(cc[b]))
    endforall
  endforall;

invariant "SPM copies are not directory participants"
  forall n: Node do
    cc[n] = CC_X -> !sharer[n] & !(owner_valid & owner = n)
  endforall;

invariant "directory sharers are stable or transient shared owners"
  forall n: Node do
    sharer[n] ->
      (cc[n] = CC_S | cc[n] = CC_SI_A | cc[n] = CC_SX_A |
       cc[n] = CC_M | cc[n] = CC_E | cc[n] = CC_MI_A | cc[n] = CC_EI_A |
       WaitingForData(cc[n]) | WaitingForAck(cc[n]) | MigratingCoherent(cc[n]) |
       HasMsg(MSG_ExclDataDir, n) | HasMsg(MSG_DataDir, n) |
       HasMsg(MSG_DataOwner, n))
  endforall;

invariant "directory owner is coherent owner or pending owner"
  owner_valid ->
    (dir = DIR_E | dir = DIR_M) &
    (StableCoherent(cc[owner]) | WaitingForData(cc[owner]) |
     WaitingForAck(cc[owner]) | MigratingCoherent(cc[owner]) |
     HasMsg(MSG_DataDir, owner) |
     HasMsg(MSG_DataOwner, owner));

invariant "invalid directory has no stable coherent participants"
  dir = DIR_I ->
    !owner_valid & CountSharers() = 0 &
    forall n: Node do
      !StableCoherent(cc[n])
    endforall;

invariant "exclusive/modified directory has no sharers"
  (dir = DIR_E | dir = DIR_M) -> CountSharers() = 0;

invariant "SD has an outstanding forwarded data transaction"
  dir = DIR_SD -> mshr_valid;

invariant "non-SD has no directory MSHR"
  dir != DIR_SD -> !mshr_valid;

invariant "ack counter only used by GetM waiters"
  forall n: Node do
    cc_ack[n] > 0 ->
      (cc[n] = CC_IM_AD | cc[n] = CC_SM_AD | cc[n] = CC_IM_A | cc[n] = CC_SM_A)
  endforall;
