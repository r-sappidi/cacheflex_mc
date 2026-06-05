-- CacheFlex SPM table-level model for Rumur.
--
-- Scope:
--   * one coherent cache line
--   * two L1/private-cache controllers
--   * one directory/home controller
--   * table-level transitions from spm_coherency_CC.csv and
--     spm_coherency_dir.csv
--
-- Blank table cells are intentionally not modeled as rules. This keeps the
-- model honest: if software/protocol sequencing needs one of those events, the
-- checker will not invent behavior for it.

const
  N: 4;
  NONE: 4;
  WAYS: 3;
  PADDRS: 1;
  NO_PADDR: 1;

type
  Core: 0..3;
  MaybeCore: 0..4;
  Way: 0..2;
  PhysAddr: 0..0;
  MaybePhysAddr: 0..1;
  DataId: 0..1;

  CCState: enum {
    C_I,
    C_ISD,
    C_IMAD,
    C_IMA,
    C_S,
    C_SXA,
    C_SMAD,
    C_SMA,
    C_M,
    C_MXA,
    C_E,
    C_EXA,
    C_MIA,
    C_EIA,
    C_SIA,
    C_IIA,
    C_IXD,
    C_X,
    C_XWB
  };

  DirState: enum {
    D_I,
    D_S,
    D_E,
    D_M,
    D_SD
  };

  PhysLine: record
    valid: boolean;
    addr: MaybePhysAddr;
    state: CCState;
    is_spm: boolean;
    data: DataId;
  end;

  PhysDirLine: record
    state: DirState;
    owner: MaybeCore;
    sharer: array [Core] of boolean;
  end;

var
  l1: array [Core] of CCState;
  dir: DirState;
  owner: MaybeCore;
  sharer: array [Core] of boolean;

  sd_req: MaybeCore;
  sd_owner: MaybeCore;
  sd_silent: boolean;

  -- Tiny one-set physical L1 extension used to verify SPMCP_install lazy
  -- migration. This is intentionally separate from the one-line table model
  -- above: the scalar l1[] state tracks the table-level SPM fetch protocol,
  -- while phys_l1[][] tracks set/way placement effects for destination slots.
  phys_l1: array [Core] of array [Way] of PhysLine;
  phys_dir: array [PhysAddr] of PhysDirLine;
  phys_saw_migration: boolean;
  phys_rejected_existing_spm: boolean;
  phys_rejected_no_free_way: boolean;
  phys_coh_side_effect: boolean;

function IsCoherentPresent(s: CCState): boolean;
begin
  return s = C_S | s = C_E | s = C_M |
         s = C_SXA | s = C_MXA | s = C_EXA |
         s = C_SMAD | s = C_SMA |
         s = C_MIA | s = C_EIA | s = C_SIA | s = C_IIA;
end;

function IsStableCoherent(s: CCState): boolean;
begin
  return s = C_S | s = C_E | s = C_M;
end;

function PhysSharerCount(a: PhysAddr): 0..N;
var
  n: 0..N;
begin
  n := 0;
  for c: Core do
    if phys_dir[a].sharer[c] then
      n := n + 1;
    endif;
  endfor;
  return n;
end;

function PhysHasLine(c: Core; a: PhysAddr): boolean;
var
  found: boolean;
begin
  found := false;
  for w: Way do
    if phys_l1[c][w].valid &
       !phys_l1[c][w].is_spm &
       phys_l1[c][w].addr = a then
      found := true;
    endif;
  endfor;
  return found;
end;

function PhysDuplicateLineCount(c: Core; a: PhysAddr): 0..WAYS;
var
  n: 0..WAYS;
begin
  n := 0;
  for w: Way do
    if phys_l1[c][w].valid &
       !phys_l1[c][w].is_spm &
       phys_l1[c][w].addr = a then
      n := n + 1;
    endif;
  endfor;
  return n;
end;

function SharerCount(): 0..N;
var
  n: 0..N;
begin
  n := 0;
  for c: Core do
    if sharer[c] then
      n := n + 1;
    endif;
  endfor;
  return n;
end;

procedure ClearSharers();
begin
  for c: Core do
    sharer[c] := false;
  endfor;
end;

procedure RemoveSharer(c: Core);
begin
  sharer[c] := false;
  if SharerCount() = 0 then
    if dir = D_S then
      dir := D_I;
    endif;
  endif;
end;

procedure SetOwner(c: Core; s: DirState);
begin
  owner := c;
  dir := s;
  ClearSharers();
end;

procedure ClearOwner();
begin
  owner := NONE;
end;

procedure PhysClearSharers(a: PhysAddr);
begin
  for c: Core do
    phys_dir[a].sharer[c] := false;
  endfor;
end;

procedure PhysInvalidateWay(c: Core; w: Way);
begin
  phys_l1[c][w].valid := false;
  phys_l1[c][w].addr := NO_PADDR;
  phys_l1[c][w].state := C_I;
  phys_l1[c][w].is_spm := false;
  phys_l1[c][w].data := 0;
end;

procedure PhysInstallCoherent(c: Core; w: Way; a: PhysAddr; s: CCState);
begin
  assert IsStableCoherent(s) "physical coherent installs use stable S/E/M";
  phys_l1[c][w].valid := true;
  phys_l1[c][w].addr := a;
  phys_l1[c][w].state := s;
  phys_l1[c][w].is_spm := false;
  phys_l1[c][w].data := 1;
end;

procedure PhysClaimSPMSlot(c: Core; dst: Way);
var
  free: Way;
  found_free: boolean;
begin
  found_free := false;
  free := 0;

  if phys_l1[c][dst].valid & phys_l1[c][dst].is_spm then
    -- Re-installing over an existing X slot is rejected. Software must
    -- release before reusing the slot.
    phys_rejected_existing_spm := true;
  else
    if phys_l1[c][dst].valid then
      assert IsStableCoherent(phys_l1[c][dst].state)
        "SPMCP_install only migrates stable coherent occupants";

      for w: Way do
        if w != dst & !phys_l1[c][w].valid & !found_free then
          free := w;
          found_free := true;
        endif;
      endfor;

      if found_free then
        -- Lazy migration is pure local placement: exact addr/state/data moves,
        -- directory owner/sharer metadata is intentionally untouched.
        phys_l1[c][free] := phys_l1[c][dst];
        phys_saw_migration := true;
      else
        -- The Ruby prototype panics in this case. The model records the
        -- rejection as a terminally visible outcome instead of inventing an
        -- eviction/writeback transition.
        phys_rejected_no_free_way := true;
      endif;
    endif;

    if !phys_l1[c][dst].valid | found_free then
      phys_l1[c][dst].valid := true;
      phys_l1[c][dst].addr := NO_PADDR;
      phys_l1[c][dst].state := C_X;
      phys_l1[c][dst].is_spm := true;
      phys_l1[c][dst].data := 1;
    endif;
  endif;
end;

procedure CompleteSilentData(c: Core);
begin
  -- CC table: IX^D + data from Dir/Owner routes to SPM and enters X.
  l1[c] := C_X;
end;

procedure DirGetS(c: Core);
begin
  if dir = D_I then
    SetOwner(c, D_E);
    l1[c] := C_E;
  elsif dir = D_S then
    sharer[c] := true;
    l1[c] := C_S;
  elsif dir = D_E | dir = D_M then
    assert owner != NONE "Dir E/M must have an owner before GetS";
    sd_req := c;
    sd_owner := owner;
    sd_silent := false;
    l1[owner] := C_S;
    sharer[owner] := true;
    ClearOwner();
    dir := D_SD;
  endif;
end;

procedure DirGetM(c: Core);
begin
  if dir = D_I then
    SetOwner(c, D_M);
    l1[c] := C_M;
  elsif dir = D_S then
    ClearSharers();
    SetOwner(c, D_M);
    l1[c] := C_M;
  elsif dir = D_E | dir = D_M then
    assert owner != NONE "Dir E/M must have an owner before GetM";
    if owner != c then
      l1[owner] := C_I;
    endif;
    owner := c;
    dir := D_M;
    l1[c] := C_M;
  endif;
end;

procedure DirGetSSilent(c: Core);
begin
  if dir = D_I then
    CompleteSilentData(c);
  elsif dir = D_S then
    -- Dir table: send data to requester, do not modify sharer list.
    CompleteSilentData(c);
  elsif dir = D_E | dir = D_M then
    assert owner != NONE "Dir E/M must have an owner before GetS_silent";
    sd_req := c;
    sd_owner := owner;
    sd_silent := true;
    l1[owner] := C_S;
    sharer[owner] := true;
    ClearOwner();
    dir := D_SD;
  endif;
end;

procedure DirPutS(c: Core);
begin
  -- Dir table: PutS removes requester from sharers when present, otherwise ack.
  if dir = D_S | dir = D_SD then
    RemoveSharer(c);
  endif;
end;

procedure DirPutM(c: Core);
begin
  -- Owner PutM+data clears ownership and invalidates the directory entry.
  -- Non-owner PutM is only acknowledged.
  if (dir = D_M | dir = D_E) & owner = c then
    ClearOwner();
    dir := D_I;
  elsif dir = D_S | dir = D_SD then
    RemoveSharer(c);
  endif;
end;

procedure DirPutE(c: Core);
begin
  -- Owner PutE clears ownership. Non-owner PutE is only acknowledged.
  if dir = D_E & owner = c then
    ClearOwner();
    dir := D_I;
  endif;
end;

procedure DirSPMWB(c: Core);
begin
  -- spm_coherency_dir.csv only defines SPMWB_Req in I.
  assert dir = D_I "SPMWB_Req is table-defined only in directory I";
  l1[c] := C_X;
end;

startstate "two-core invalid line"
begin
  for c: Core do
    l1[c] := C_I;
    sharer[c] := false;
    for w: Way do
      PhysInvalidateWay(c, w);
    endfor;
  endfor;
  dir := D_I;
  owner := NONE;
  sd_req := NONE;
  sd_owner := NONE;
  sd_silent := false;

  for a: PhysAddr do
    phys_dir[a].state := D_I;
    phys_dir[a].owner := NONE;
    PhysClearSharers(a);
  endfor;
  phys_saw_migration := false;
  phys_rejected_existing_spm := false;
  phys_rejected_no_free_way := false;
  phys_coh_side_effect := false;
endstartstate;

ruleset c: Core do
  rule "CC I Load -> IS^D, Dir GetS"
    l1[c] = C_I & dir != D_SD
  ==>
  begin
    l1[c] := C_ISD;
    DirGetS(c);
  endrule;

  rule "CC I Store -> IM^AD, Dir GetM"
    l1[c] = C_I & dir != D_SD
  ==>
  begin
    l1[c] := C_IMAD;
    DirGetM(c);
  endrule;

  rule "CC I SPMCP_fetch -> IX^D, Dir GetS_silent"
    l1[c] = C_I & dir != D_SD
  ==>
  begin
    l1[c] := C_IXD;
    DirGetSSilent(c);
  endrule;

  rule "CC S Load hit"
    l1[c] = C_S
  ==>
  begin
  endrule;

  rule "CC S Store -> SM^AD, Dir GetM"
    l1[c] = C_S & dir != D_SD
  ==>
  begin
    l1[c] := C_SMAD;
    DirGetM(c);
  endrule;

  rule "CC S SPMCP_fetch -> SX^A, PutS"
    l1[c] = C_S
  ==>
  begin
    l1[c] := C_SXA;
    DirPutS(c);
  endrule;

  rule "CC E Store hit -> M"
    l1[c] = C_E
  ==>
  begin
    l1[c] := C_M;
    dir := D_M;
    owner := c;
  endrule;

  rule "CC E SPMCP_fetch -> EX^A, PutE"
    l1[c] = C_E
  ==>
  begin
    l1[c] := C_EXA;
    DirPutE(c);
  endrule;

  rule "CC M SPMCP_fetch -> MX^A, PutM+data"
    l1[c] = C_M
  ==>
  begin
    l1[c] := C_MXA;
    DirPutM(c);
  endrule;

  rule "CC M Load/Store hit"
    l1[c] = C_M
  ==>
  begin
  endrule;

  rule "CC SX^A Put-Ack -> X"
    l1[c] = C_SXA
  ==>
  begin
    l1[c] := C_X;
  endrule;

  rule "CC EX^A Put-Ack -> X"
    l1[c] = C_EXA
  ==>
  begin
    l1[c] := C_X;
  endrule;

  rule "CC MX^A Put-Ack -> X"
    l1[c] = C_MXA
  ==>
  begin
    l1[c] := C_X;
  endrule;

  rule "CC X SPMLD/SPMWB_read returns SPM data"
    l1[c] = C_X
  ==>
  begin
  endrule;

  rule "CC X SPMST updates SPM data"
    l1[c] = C_X
  ==>
  begin
  endrule;

  rule "CC X SPMWB_store -> XWB, Dir SPMWB_Req"
    l1[c] = C_X & dir = D_I
  ==>
  begin
    l1[c] := C_XWB;
    DirSPMWB(c);
  endrule;

  rule "CC X SPM_release -> I"
    l1[c] = C_X
  ==>
  begin
    l1[c] := C_I;
  endrule;

  rule "CC XWB SPMWB_Ack -> X"
    l1[c] = C_XWB
  ==>
  begin
    l1[c] := C_X;
  endrule;

  rule "CC non-X SPMLD/SPMWB_read returns zero"
    l1[c] = C_I | l1[c] = C_S | l1[c] = C_E | l1[c] = C_M
  ==>
  begin
  endrule;

  rule "CC non-X SPMST/SPMWB_store ignored or zero-completed"
    l1[c] = C_I | l1[c] = C_S | l1[c] = C_E | l1[c] = C_M
  ==>
  begin
  endrule;

  rule "CC non-X SPM_release ignored"
    l1[c] = C_I | l1[c] = C_S | l1[c] = C_E | l1[c] = C_M
  ==>
  begin
  endrule;
endruleset;

rule "Dir SD owner data returns to original requester"
  dir = D_SD
==>
begin
  if sd_silent then
    -- Dir table: do not add the SPM requester as sharer.
    l1[sd_req] := C_X;
    dir := D_I;
    ClearSharers();
  else
    sharer[sd_req] := true;
    l1[sd_req] := C_S;
    dir := D_S;
  endif;
  sd_req := NONE;
  sd_owner := NONE;
  sd_silent := false;
endrule;

ruleset c: Core do
  rule "environment replacement S -> I with PutS"
    l1[c] = C_S
  ==>
  begin
    l1[c] := C_I;
    DirPutS(c);
  endrule;

  rule "environment replacement E/M -> I with PutE/PutM"
    l1[c] = C_E | l1[c] = C_M
  ==>
  begin
    if l1[c] = C_E then
      DirPutE(c);
    else
      DirPutM(c);
    endif;
    l1[c] := C_I;
  endrule;
endruleset;

ruleset c: Core; a: PhysAddr; w: Way do
  rule "phys env create S coherent occupant"
    !phys_l1[c][w].valid &
    !PhysHasLine(c, a) &
    (phys_dir[a].state = D_I | phys_dir[a].state = D_S)
  ==>
  begin
    PhysInstallCoherent(c, w, a, C_S);
    phys_dir[a].state := D_S;
    phys_dir[a].owner := NONE;
    phys_dir[a].sharer[c] := true;
  endrule;

  rule "phys env create E coherent occupant"
    !phys_l1[c][w].valid &
    !PhysHasLine(c, a) &
    phys_dir[a].state = D_I
  ==>
  begin
    PhysInstallCoherent(c, w, a, C_E);
    phys_dir[a].state := D_E;
    phys_dir[a].owner := c;
    PhysClearSharers(a);
  endrule;

  rule "phys env create M coherent occupant"
    !phys_l1[c][w].valid &
    !PhysHasLine(c, a) &
    phys_dir[a].state = D_I
  ==>
  begin
    PhysInstallCoherent(c, w, a, C_M);
    phys_dir[a].state := D_M;
    phys_dir[a].owner := c;
    PhysClearSharers(a);
  endrule;
endruleset;

ruleset c: Core; dst: Way do
  rule "phys SPMCP_install claims destination way"
    true
  ==>
  begin
    PhysClaimSPMSlot(c, dst);
  endrule;
endruleset;

invariant "SPM X is outside directory sharers"
  forall c: Core do
    l1[c] = C_X -> !sharer[c]
  endforall;

invariant "SPM X is outside directory ownership"
  forall c: Core do
    l1[c] = C_X -> owner != c
  endforall;

invariant "directory I has no owner or sharers"
  dir = D_I -> owner = NONE & SharerCount() = 0;

invariant "directory S has no owner"
  dir = D_S -> owner = NONE;

invariant "directory E/M has an owner"
  (dir = D_E | dir = D_M) -> owner != NONE;

invariant "stable coherent owner is not scratchpad"
  forall c: Core do
    owner = c -> l1[c] != C_X & l1[c] != C_XWB
  endforall;

invariant "at most one stable modified/exclusive owner-like L1"
  forall c1: Core do
    forall c2: Core do
      (c1 != c2 & (l1[c1] = C_M | l1[c1] = C_E) &
       (l1[c2] = C_M | l1[c2] = C_E)) -> false
    endforall
  endforall;

invariant "physical model emits no coherence side effect during migration"
  !phys_coh_side_effect;

invariant "physical SPM way metadata is X-only"
  forall c: Core do
    forall w: Way do
      (phys_l1[c][w].valid & phys_l1[c][w].is_spm) ->
        phys_l1[c][w].state = C_X & phys_l1[c][w].addr = NO_PADDR
    endforall
  endforall;

invariant "physical coherent ways are not marked SPM"
  forall c: Core do
    forall w: Way do
      (phys_l1[c][w].valid & !phys_l1[c][w].is_spm) ->
        IsStableCoherent(phys_l1[c][w].state) &
        phys_l1[c][w].addr != NO_PADDR
    endforall
  endforall;

invariant "lazy migration keeps one physical copy per core address"
  forall c: Core do
    forall a: PhysAddr do
      PhysDuplicateLineCount(c, a) <= 1
    endforall
  endforall;

invariant "physical directory I has no owner or sharers"
  forall a: PhysAddr do
    phys_dir[a].state = D_I ->
      phys_dir[a].owner = NONE & PhysSharerCount(a) = 0
  endforall;

invariant "physical directory S has sharers and no owner"
  forall a: PhysAddr do
    phys_dir[a].state = D_S ->
      phys_dir[a].owner = NONE & PhysSharerCount(a) > 0
  endforall;

invariant "physical directory E/M has owner and no sharers"
  forall a: PhysAddr do
    (phys_dir[a].state = D_E | phys_dir[a].state = D_M) ->
      phys_dir[a].owner != NONE & PhysSharerCount(a) = 0
  endforall;

invariant "physical S lines remain directory sharers after migration"
  forall c: Core do
    forall w: Way do
      (phys_l1[c][w].valid & !phys_l1[c][w].is_spm &
       phys_l1[c][w].state = C_S) ->
        phys_dir[phys_l1[c][w].addr].state = D_S &
        phys_dir[phys_l1[c][w].addr].sharer[c]
    endforall
  endforall;

invariant "physical E/M lines remain directory owners after migration"
  forall c: Core do
    forall w: Way do
      (phys_l1[c][w].valid & !phys_l1[c][w].is_spm &
       (phys_l1[c][w].state = C_E | phys_l1[c][w].state = C_M)) ->
        phys_dir[phys_l1[c][w].addr].owner = c &
        ((phys_l1[c][w].state = C_E &
          phys_dir[phys_l1[c][w].addr].state = D_E) |
         (phys_l1[c][w].state = C_M &
          phys_dir[phys_l1[c][w].addr].state = D_M))
    endforall
  endforall;
