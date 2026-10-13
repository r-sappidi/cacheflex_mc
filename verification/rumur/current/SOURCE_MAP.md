# Retained-source correspondence update (2026-10-01)

- `L0cache.sm:spm_issueRequestToL1`: valid L0 source captured into existing
  CoherenceMsg DataBlk; SPMSnapshot marks it and MessageSize is Data.
- `L1cache.sm:SPMCP_snapshot`: installs buffered payload, preserving stable
  coherent state; normal snoops/evictions can race transport.
- `L1cache.sm:SS/EE/MM + SPMCP_fetch`: buffer source before lazy slot migration,
  install SPM, acknowledge without source PUT or coherent deallocation.
- L0 recall fallback completes in SS/EE/MM instead of release/Put_Ack states.
- GETS_SILENT and SPMWB source/LLC/directory paths are unchanged.
- The old local PUT states/handlers remain as legacy definitions but are not
  entered by retained-source copy paths. Model RETAIN_SOURCE=0 is diagnostic only.

The correspondence table below describes v5 history where source release applied;
its release row is superseded by the retained-source mapping above.

# Implementation correspondence and abstraction limits

The manifest pins the exact checked source files; line numbers below are discovery
anchors, not a claim that all controller behavior is reproduced.

| Model behavior | Implementation anchor |
|---|---|
| Local-source PUT/ack conversion and forwarded requests while waiting | `MESI_Three_Level_SPM-L1cache.sm`: `SPMCP_fetch` transitions to `SX_A/EX_A/MX_A`; `Put_Ack`; `IX_A`; handlers around lines 2085–2438 |
| Stale PUT does not overwrite a newer coherent owner | `MESI_Three_Level_SPM-L2cache.sm`: `request_type_to_event`, `SPM_PUT_old` |
| Silent NACK plus existing-TBE reissue | private L1 controller: `spm_nackSilentGetS`, `spm_rememberSilentRetry`, `spm_reissueSilentGetS` |
| Writeback invalidation then installation | LLC controller: `SPM_WB_INV`, `swb_allocateInvTBE`, `swb_writeDataFromTBE` |
| Retained LLC fill records directory ownership | LLC controller: `sa_issueSilentFetchToMemory`, `Mem_Data_SPM_Retain` |
| Transient relocation | `CacheMemory.cc`: `migrateOrClearSPMSlot`; state remains address-keyed |
| Existing request-VC class partition | Garnet `NetworkInterface.cc` and `OutputUnit.cc`; `cacheflex_request_vc_partition` |
| Reduction completion, pending-row hazards, bank serialization | LLC controller L3 reduction service and `CacheMemory.cc` reduction/pending bookkeeping |
| CPU-side source ordering before finalization/writeback | `lsq_unit.cc` source-range overlap checks, including older SPM stores/bridge shadows |

Historical F1–F4/F7/F8 repaired branches are enabled as
`IMPLEMENTED_TRANSITIONS=1`; historical disable branches remain only to make
comparison with the preserved model straightforward. The primary new behavioral
negative control is `WB_INVALIDATE=0`. The source/escape return paths of the
finite model are folded into the abstract L0 FIFO; explicit queue-cycle proof is
outside the private model. Multicast, prefetch/instruction-fetch interactions,
multiple lines/banks, speculative vector-address formation, and the complete
CPU instruction pipeline are not modeled here.

Do not interpret these tests as verifying the recent Q/SPM vector boundary fix:
that is a guest-address-generation issue covered by separate kernel tests.
