-- Directed finite-buffer dependency abstraction, NOT a Garnet mesh model.
-- Two historical wait cycles, with same bounded storage in all variants.
const
 RETRY: 1;
 PARTITION: 1;
type Phase: enum {Blocked, Response, Captured, ForwardFreed, Settled, Drained};
var phase: Phase; scenario: 0..1; miss_credit: boolean; retry_record: boolean;
    forward_full: boolean; private_full: boolean; directory_full: boolean;
ruleset s: 0..1 do
 startstate
  scenario:=s; phase:=Blocked; miss_credit:=true; retry_record:=false;
  forward_full:=true; private_full:=true; directory_full:=true;
 endstartstate;
endruleset;
-- Scenario0: forward handler cannot bounce into the full request FIFO. NACK
-- has a separate response path and is consumed into the existing miss TBE.
rule "NACK escapes blocked request FIFO"
 scenario=0 & phase=Blocked & RETRY=1
==> begin phase:=Response; forward_full:=false; endrule;
rule "consume NACK without request space"
 phase=Response
==> begin assert miss_credit; retry_record:=true; phase:=Captured; endrule;
rule "released forward FIFO permits queued request to drain"
 phase=Captured
==> begin private_full:=false; phase:=ForwardFreed; endrule;
rule "owner writeback settles at home"
 scenario=0 & phase=ForwardFreed
==> begin directory_full:=false; phase:=Settled; endrule;
rule "retry original transaction after request space becomes available"
 scenario=0 & phase=Settled
==> begin assert retry_record & miss_credit; retry_record:=false;
 miss_credit:=false; phase:=Drained; endrule;
-- Scenario1: directory-destination packet requires a VC held by LLC-bound
-- traffic, whose next admission depends on that directory completion. A
-- partition of EXISTING VCs prevents that class from occupying both classes.
rule "directory class retains independently allocatable VC"
 scenario=1 & phase=Blocked & PARTITION=1
==> begin directory_full:=false; phase:=Settled; endrule;
rule "directory completion frees stalled LLC-bound traffic"
 scenario=1 & phase=Settled
==> begin private_full:=false; forward_full:=false; miss_credit:=false; phase:=Drained; endrule;
-- Self-loop keeps a blocked state explicit; liveness must detect it.
rule "environment tick" true ==> begin phase:=phase; endrule;
invariant "retry never allocates another miss credit" retry_record -> miss_credit;
invariant "completion drains all modeled resources"
 phase=Drained -> (!miss_credit & !retry_record & !forward_full & !private_full & !directory_full);
liveness "both directed dependencies can drain" phase=Drained;
