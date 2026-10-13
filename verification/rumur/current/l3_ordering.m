-- Bounded command/slot lifecycle abstraction. Arithmetic is symbolic integer
-- update; this checks ordering, not FP16/FP32 numerical correctness or timing.
const SOURCE_RAW: 1; PENDING_RELEASE: 1; SINK_ORDER: 1;
type Core:0..1; Cmd:0..2; State:enum{Queued,Captured,Flight,Sink,Done};
var st:array[Core] of array[Cmd] of State;
 claimed:array[Core] of boolean; owner:array[Core] of Core;
 pending:array[Core] of 0..3; value:array[Core] of 0..3;
 installed:array[Core] of boolean; exported:array[Core] of boolean;
 wb_wait:array[Core] of boolean; stale_reader:array[Core] of boolean;
 released:array[Core] of boolean;
function sinks():0..6;
var n:0..6;
begin
 n:=0;
 for c:Core do for k:Cmd do if st[c][k]=Sink then n:=n+1;endif;endfor;endfor;
 return n;
end;
startstate
 for c:Core do
  for k:Cmd do st[c][k]:=Queued; endfor;
  claimed[c]:=true;owner[c]:=c;pending[c]:=0;value[c]:=0;
  installed[c]:=false;exported[c]:=false;wb_wait[c]:=false;
  stale_reader[c]:=true;released[c]:=false;
 endfor;
endstartstate;
ruleset c:Core; k:Cmd do
 rule "capture into bounded source records"
 st[c][k]=Queued & claimed[c] & pending[c]<3 &
 (k=0 | st[c][k-1]!=Queued) &
 (k!=2 | SOURCE_RAW=0 | (st[c][0]=Done & st[c][1]=Done))
 ==> begin st[c][k]:=Captured;pending[c]:=pending[c]+1;endrule;
 rule "transport accepts real contribution and returns early source ACK"
 st[c][k]=Captured
 ==> begin st[c][k]:=Flight;endrule;
 rule "bounded bank sink accepts command"
 st[c][k]=Flight & sinks()<4
 ==> begin st[c][k]:=Sink;endrule;
 rule "bank completes initialization or update"
 k<2 & st[c][k]=Sink &
 (SINK_ORDER=0 | k=0 | st[c][k-1]=Done)
 ==> begin
  assert claimed[c] & owner[c]=c "update reached released or reassigned slot";
  if k=0 then installed[c]:=true;value[c]:=1;
  else assert installed[c] "update before initialization";value[c]:=value[c]+1;endif;
  st[c][k]:=Done;pending[c]:=pending[c]-1;
 endrule;
 rule "finalization starts coherent invalidation"
 k=2 & st[c][k]=Sink & !wb_wait[c]
 ==> begin
  assert st[c][0]=Done & st[c][1]=Done "finalization overtook older reduction";
  assert value[c]=2 "finalization exported wrong abstract reduction";
  wb_wait[c]:=true;
 endrule;
endruleset;
ruleset c:Core do
 rule "coherent destination invalidation acknowledged"
 wb_wait[c] & stale_reader[c]
 ==> begin stale_reader[c]:=false;endrule;
 rule "directory memory ACK completes writeback"
 wb_wait[c] & !stale_reader[c] & st[c][2]=Sink
 ==> begin exported[c]:=true;st[c][2]:=Done;pending[c]:=pending[c]-1;endrule;
 rule "release and synchronized transfer to next owner"
 !released[c] & (PENDING_RELEASE=0 | (pending[c]=0 & st[c][2]=Done))
 ==> begin claimed[c]:=false;released[c]:=true;owner[c]:=1-c;endrule;
endruleset;
rule "environment tick" true ==> begin claimed[0]:=claimed[0];endrule;
invariant "no early release"
 forall c:Core do released[c] -> (pending[c]=0 & st[c][2]=Done) endforall;
invariant "coherent visibility follows invalidation"
 forall c:Core do exported[c] -> !stale_reader[c] endforall;
invariant "pending bookkeeping"
 forall c:Core do pending[c]=
 ( (st[c][0]!=Queued & st[c][0]!=Done) ? 1:0 )+
 ( (st[c][1]!=Queued & st[c][1]!=Done) ? 1:0 )+
 ( (st[c][2]!=Queued & st[c][2]!=Done) ? 1:0 ) endforall;
liveness "all commands and handoffs can complete" released[0] & released[1];

invariant "four bank sink records" sinks()<=4;
