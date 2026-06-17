#!/usr/bin/env python3
"""Generate the SPM coherency state-transition CSVs to match the SLICC
implementation.

Tables stay in the textbook MSI/MESI notation of the original design doc, but
the cells are reconciled against the gem5 protocol files:

  spm_coherency_CC.csv  <-  MESI_Three_Level_SPM-L1cache.sm (SPM cache ctrl)
  spm_coherency_dir.csv <-  MESI_Three_Level_SPM-L2cache.sm (SPM directory/LLC)

Textbook<->SLICC state mapping
  CC : S/E/M = line present in L1 *and* L0; the SS/EE/MM "L1-only" variants are
       folded into the same textbook row (noted in each SPMCP_fetch cell).
       SX^L0/EX^L0/MX^L0 are the L0-recall wait states; *X^A wait for Put-Ack;
       IX^D waits for the silent fetch; X is the live scratchpad slot.
  dir: I=NP, S=SS(shared), E/M=owner present in a local L1 (MT),
       SD=MT_SPMS (silent GetS forwarded to owner, awaiting Unblock).
"""
import csv
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --------------------------------------------------------------------------
# CC (L1 SPM cache controller)
# --------------------------------------------------------------------------
CC_COLS = [
    "State", "Load", "SPMCP_fetch", "SPMCP_install", "SPMWB_store", "Store",
    "SPMWB_read", "SPM_release", "SPMLD", "SPMST", "SPMWB_Ack", "Replacement",
    "Fwd-GetS", "Fwd-GetS_Silent", "Fwd-GetM", "Inv", "Put-Ack",
    "Exclusive Data from Dir", "Data from Dir (ack=0)", "Data from Dir (ack>0)",
    "Data from Owner", "Inv-Ack", "Last-Inv-Ack", "L0 recall resp",
]

# SPM L0-side passthrough for the stable coherent states I/S/E/M:
#   SPMLD/SPMWB_read -> return zero ; SPMST/SPMWB_store/SPM_release -> ack ;
#   SPMCP_install    -> claim slot /X
SPM_PASS = {
    "SPMCP_install": "Claim SPM slot, ack L0 /X",
    "SPMWB_store": "Ack L0",
    "SPMWB_read": "Return zero to L0",
    "SPM_release": "Ack L0",
    "SPMLD": "Return zero to L0",
    "SPMST": "Ack L0",
}
# Transient coherence states stall every SPM L0 op until the miss resolves.
SPM_STALL = {k: "Stall" for k in
             ("SPMCP_fetch", "SPMCP_install", "SPMWB_store", "SPMWB_read",
              "SPM_release", "SPMLD", "SPMST")}
# Local CPU/SPM ops + forwards all stall in the SPM wait states.
LOCAL_STALL = {k: "Stall" for k in
               ("Load", "Store", "Replacement", "SPMCP_fetch", "SPMCP_install",
                "SPMWB_store", "SPMWB_read", "SPM_release", "SPMLD", "SPMST")}
FWD_STALL = {k: "Stall" for k in
             ("Fwd-GetS", "Fwd-GetS_Silent", "Fwd-GetM", "Inv")}

cc_rows = []

def cc(state, **cells):
    row = {c: "" for c in CC_COLS}
    row["State"] = state
    row.update(cells)
    cc_rows.append(row)

# ---- stable coherent states (also the SPM-fetch sources) ----
cc("I", Load="Send GetS to Dir /IS^D",
   SPMCP_fetch="Alloc TBE, send GetS_Silent to Dir /IX^D",
   Store="Send GetM to Dir /IM^AD", **SPM_PASS)

cc("IS^D", Load="Stall", Store="Stall", Replacement="Stall", Inv="Stall",
   **{"Exclusive Data from Dir": "-/E", "Data from Dir (ack=0)": "-/S",
      "Data from Owner": "-/S"}, **SPM_STALL)

cc("IM^AD", Load="Stall", Store="Stall", Replacement="Stall",
   **{"Fwd-GetS": "Stall", "Fwd-GetM": "Stall", "Data from Dir (ack=0)": "-/M",
      "Data from Dir (ack>0)": "-/IM^A", "Data from Owner": "-/M",
      "Inv-Ack": "ack--"}, **SPM_STALL)

cc("IM^A", Load="Stall", Store="Stall", Replacement="Stall",
   **{"Fwd-GetS": "Stall", "Fwd-GetM": "Stall", "Inv-Ack": "ack--",
      "Last-Inv-Ack": "-/M"}, **SPM_STALL)

cc("S", Load="Hit",
   SPMCP_fetch="Recall L0 copy /SX^L0 then send PutS to Dir /SX^A "
               "(SS: send PutS directly /SX^A)",
   Store="Send GetM to Dir /SM^AD", Replacement="Send PutS to Dir /SI^A",
   Inv="Send Inv-Ack to Req /I", **SPM_PASS)

cc("SX^L0", **LOCAL_STALL, **FWD_STALL,
   **{"L0 recall resp": "On L0_Ack: send PutS to Dir /SX^A"})

cc("SX^A", **LOCAL_STALL,
   **{"Fwd-GetS": "Stall", "Fwd-GetM": "Stall", "Inv": "Stall",
      "Put-Ack": "Install SPM data from cache, ack L0 /X"})

cc("SM^AD", Load="Hit", Store="Stall", Replacement="Stall",
   **{"Fwd-GetS": "Stall", "Fwd-GetM": "Stall",
      "Inv": "Send Inv-Ack to Req /IM^AD", "Data from Dir (ack=0)": "-/M",
      "Data from Dir (ack>0)": "-/SM^A", "Inv-Ack": "ack--"}, **SPM_STALL)

cc("SM^A", Load="Hit", Store="Stall", Replacement="Stall",
   **{"Fwd-GetS": "Stall", "Fwd-GetM": "Stall", "Inv-Ack": "ack--",
      "Last-Inv-Ack": "-/M"}, **SPM_STALL)

cc("M", Load="Hit",
   SPMCP_fetch="Recall L0 dirty data /MX^L0 then send PutM+data to Dir /MX^A "
               "(MM: send PutM+data directly /MX^A)",
   Store="Hit", Replacement="Send PutM+data to Dir /MI^A",
   **{"Fwd-GetS": "Send data to Req and Dir /S",
      "Fwd-GetM": "Send data to Req /I"}, **SPM_PASS)

cc("MX^L0", **LOCAL_STALL, **FWD_STALL,
   **{"L0 recall resp": "On L0_DataAck/Copy: write back L0 dirty data + send "
      "PutM+data /MX^A; on L0_DataNak: send PutM+data /MX^A"})

cc("MX^A", **LOCAL_STALL,
   **{"Fwd-GetS": "Send data to Req and Dir, downgrade /SX^A",
      "Fwd-GetS_Silent": "Send silent data copy to Req, Unblock L2 (stay)",
      "Fwd-GetM": "Stall", "Inv": "Stall",
      "Put-Ack": "Install SPM data from cache, ack L0 /X"})

cc("E", Load="Hit",
   SPMCP_fetch="Recall L0 copy /EX^L0 then send PutE to Dir /EX^A "
               "(EE: send PutE directly /EX^A)",
   Store="Hit /M", Replacement="Send PutE to Dir /EI^A",
   **{"Fwd-GetS": "Send data to Req and Dir /S",
      "Fwd-GetM": "Send data to Req /I"}, **SPM_PASS)

cc("EX^L0", **LOCAL_STALL, **FWD_STALL,
   **{"L0 recall resp": "On L0_Ack: send PutE to Dir /EX^A"})

cc("EX^A", **LOCAL_STALL,
   **{"Fwd-GetS": "Send data to Req and Dir, downgrade /SX^A",
      "Fwd-GetS_Silent": "Send silent data copy to Req, Unblock L2 (stay)",
      "Fwd-GetM": "Stall", "Inv": "Stall",
      "Put-Ack": "Install SPM data from cache, ack L0 /X"})

cc("MI^A", Load="Stall", Store="Stall", Replacement="Stall",
   **{"Fwd-GetS": "Send data to Req and Dir /SI^A",
      "Fwd-GetM": "Send data to Req /II^A", "Put-Ack": "-/I"}, **SPM_STALL)

cc("EI^A", Load="Stall", Store="Stall", Replacement="Stall",
   **{"Fwd-GetS": "Send data to Req and Dir /SI^A",
      "Fwd-GetM": "Send data to Req /II^A", "Put-Ack": "-/I"}, **SPM_STALL)

cc("SI^A", Load="Stall", Store="Stall", Replacement="Stall",
   **{"Inv": "Send Inv-Ack to Req /II^A", "Put-Ack": "-/I"}, **SPM_STALL)

cc("II^A", Load="Stall", Store="Stall", Replacement="Stall",
   **{"Put-Ack": "-/I"}, **SPM_STALL)

cc("IX^D", **LOCAL_STALL, **FWD_STALL,
   **{"Exclusive Data from Dir": "Buffer data, install to SPM, ack L0 /X",
      "Data from Dir (ack=0)": "Buffer data, install to SPM, ack L0 /X",
      "Data from Owner": "Buffer data, install to SPM, ack L0 /X"})

cc("X", SPMLD="Return SPM data to L0 /X",
   SPMWB_read="Return SPM data to L0 /X", SPMST="Update SPM data, ack L0 /X",
   SPMWB_store="Send SPMWB_Req(addr+data) to Dir /XWB",
   SPM_release="Drop SPM data, clear scratchpad bit, ack L0 /I",
   Replacement="Stall",
   **{"Fwd-GetS": "Stall", "Fwd-GetM": "Stall", "Inv": "Stall"})

cc("XWB", Load="Stall", Store="Stall", Replacement="Stall", SPMLD="Stall",
   SPMST="Stall", SPMWB_store="Stall", SPMWB_read="Stall", SPM_release="Stall",
   SPMWB_Ack="Ack L0 /X",
   **{"Fwd-GetS": "Stall", "Fwd-GetS_Silent": "Stall", "Fwd-GetM": "Stall",
      "Inv": "Stall"})

CC_LEGEND = [
    [],
    ["Legend (textbook notation; SLICC source MESI_Three_Level_SPM-L1cache.sm)"],
    ["X", "Live private-L1 scratchpad slot, outside the coherence domain"],
    ["SX^L0/EX^L0/MX^L0", "Recalling the inclusive L0 copy before relinquishing "
     "the coherent line (forward_eviction_to_L0_own); PUT issued on L0 response"],
    ["SX^A/EX^A/MX^A", "Sent PutS/PutE/PutM for own SPMCP_fetch; waiting for "
     "Put-Ack, then install from cache /X"],
    ["IX^D", "SPMCP_fetch with no local copy; issued silent GetS, routes data "
     "straight to SPM and keeps the line invalid in the coherence domain"],
    ["XWB", "SPM writeback (SPMWB_Req) outstanding to the directory"],
    ["Fwd-GetS_Silent", "Snapshot read for another core's SPMCP_fetch; served "
     "by MM/EE/MX^A/EX^A (data + Unblock, state unchanged), stalled while the "
     "freshest data has not settled (SX^L0/EX^L0/MX^L0/IX^D/XWB)"],
    ["Note", "MX^A/EX^A serve a normal Fwd-GetS by handing data to the "
     "requester + L2 and downgrading to SX^A; Fwd-GetM/Inv still stall until "
     "this core's own PUT drains to X"],
]

with open(os.path.join(ROOT, "spm_coherency_CC.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(CC_COLS)
    for r in cc_rows:
        w.writerow([r[c] for c in CC_COLS])
    for line in CC_LEGEND:
        w.writerow(line)

# --------------------------------------------------------------------------
# dir (L2/LLC SPM directory)
# --------------------------------------------------------------------------
DIR_COLS = [
    "State", "SPMWB_Req", "GetS_silent", "GetS", "GetM", "PutS-NotLast",
    "PutS-Last", "PutM+data from Owner", "PutM from Non-Owner",
    "PutE (no-data) from Owner", "PutE from Non-Owner", "SPM_PutS/PutE",
    "SPM_PutM", "Unblock", "Data",
]

SPMWB_REQ = ("Alloc + write data to LLC, clear SPM owner, issue WB to memory "
             "/SPM_WB; on memory Ack send SPMWB_Ack to L1, -> M")

dir_rows = []

def d(state, **cells):
    row = {c: "" for c in DIR_COLS}
    row["State"] = state
    row.update(cells)
    dir_rows.append(row)

d("I", SPMWB_Req=SPMWB_REQ,
  GetS_silent="Miss: issue silent fetch to memory /SPM_IS; on Mem_Data send "
              "snapshot to Req and drop block (-> I); no owner/sharer set",
  GetS="Send Exclusive data to Req, set Owner to Req/E",
  GetM="Send data to Req, set Owner to Req/M",
  **{"PutS-NotLast": "Send Put-Ack to Req", "PutS-Last": "Send Put-Ack to Req",
     "PutM from Non-Owner": "Send Put-Ack to Req",
     "PutE from Non-Owner": "Send Put-Ack to Req"})

d("S", SPMWB_Req=SPMWB_REQ,
  GetS_silent="Send SPM snapshot to Req; do NOT add sharer (stay S)",
  GetS="Send data to Req, add Req to Sharers",
  GetM="Send data to Req, send Inv to Sharers, clear Sharers, set Owner to Req/M",
  **{"PutS-NotLast": "Remove Req from Sharers, send Put-Ack to Req",
     "PutS-Last": "Remove Req from Sharers, send Put-Ack to Req/I",
     "PutM from Non-Owner": "Remove Req from Sharers, send Put-Ack to Req",
     "PutE from Non-Owner": "Remove Req from Sharers, send Put-Ack to Req",
     "SPM_PutS/PutE": "Remove Req from Sharers, send Put-Ack (stay S)",
     "SPM_PutM": "Remove Req from Sharers, write data to LLC, send Put-Ack "
                 "(stay S)"})

d("E", SPMWB_Req=SPMWB_REQ,
  GetS_silent="Forward silent GetS to Owner /SD; owner keeps the line; "
              "requester NOT added as a sharer",
  GetS="Forward GetS to Owner, make Owner sharer, add Req to Sharers, clear "
       "Owner /SD",
  GetM="Forward GetM to Owner, set Owner to Req/M",
  **{"PutS-NotLast": "Send Put-Ack to Req", "PutS-Last": "Send Put-Ack to Req",
     "PutM+data from Owner": "Copy data to mem, send Put-Ack to Req, clear Owner/I",
     "PutM from Non-Owner": "Send Put-Ack to Req",
     "PutE (no-data) from Owner": "Send Put-Ack to Req, clear Owner/I",
     "PutE from Non-Owner": "Send Put-Ack to Req",
     "SPM_PutS/PutE": "Clear SPM owner, send Put-Ack (/M, owner cleared)",
     "SPM_PutM": "Clear SPM owner, write data to LLC, send Put-Ack "
                 "(/M, owner cleared)"})

d("M", SPMWB_Req=SPMWB_REQ,
  GetS_silent="Forward silent GetS to Owner /SD; owner keeps the line; "
              "requester NOT added as a sharer",
  GetS="Forward GetS to Owner, make Owner sharer, add Req to Sharers, clear "
       "Owner /SD",
  GetM="Forward GetM to Owner, set Owner to Req/M",
  **{"PutS-NotLast": "Send Put-Ack to Req", "PutS-Last": "Send Put-Ack to Req",
     "PutM+data from Owner": "Copy data to mem, send Put-Ack to Req, clear Owner/I",
     "PutM from Non-Owner": "Send Put-Ack to Req",
     "PutE from Non-Owner": "Send Put-Ack to Req",
     "SPM_PutS/PutE": "Clear SPM owner, send Put-Ack (/M, owner cleared)",
     "SPM_PutM": "Clear SPM owner, write data to LLC, send Put-Ack "
                 "(/M, owner cleared)"})

d("SD", SPMWB_Req="Stall", GetS_silent="Stall", GetS="Stall", GetM="Stall",
  **{"PutS-NotLast": "Stall", "PutS-Last": "Stall",
     "PutM+data from Owner": "Stall", "PutM from Non-Owner": "Stall",
     "PutE (no-data) from Owner": "Stall", "PutE from Non-Owner": "Stall",
     "SPM_PutS/PutE": "Stall", "SPM_PutM": "Stall",
     "Unblock": "Owner completed silent snapshot -> E/M (owner unchanged); "
                "requester not a sharer"})

DIR_LEGEND = [
    [],
    ["Legend (textbook notation; SLICC source MESI_Three_Level_SPM-L2cache.sm)"],
    ["I = NP", "no copy in LLC"],
    ["S", "shared, LLC has data + sharer list (SS)"],
    ["E / M", "line owned by a local L1 (MT); E=clean, M=dirty owner"],
    ["SD", "MT_SPMS: silent GetS forwarded to the owner, awaiting the owner's "
     "Unblock"],
    ["SPM_PutS/PutE/PutM", "an L1 relinquishing its coherent copy to claim its "
     "SPM slot (clears the SPM owner; line stays cached in the LLC)"],
    ["GetS_silent", "snapshot fetch for an L1 SPMCP_fetch; the requester is "
     "never added as a sharer (the line leaves the coherence domain)"],
    ["SPMWB_Req", "SPM dirty-slot writeback; data goes to memory via SPM_WB "
     "before the L1 is acked"],
]

with open(os.path.join(ROOT, "spm_coherency_dir.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(DIR_COLS)
    for r in dir_rows:
        w.writerow([r[c] for c in DIR_COLS])
    for line in DIR_LEGEND:
        w.writerow(line)

print("wrote spm_coherency_CC.csv ({} state rows) and "
      "spm_coherency_dir.csv ({} state rows)".format(len(cc_rows), len(dir_rows)))
