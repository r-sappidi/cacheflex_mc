# CacheFlex SPM Rumur Model

This directory contains Murphi/Rumur models for the CacheFlex SPM state tables.

The model is intentionally table-level rather than a Ruby implementation model:

- `cacheflex_spm_tables_complete.m` is generated from the CSVs by
  `gen_table_model.py`. It emits one Rumur rule for each non-empty table cell
  up to the first blank row. Blank cells have no rule.
- `cacheflex_spm_2core.m` is a hand-written four-core safety model for one
  cache line, private L1 controllers, and one directory/home controller.
  It also includes a tiny one-set physical L1 extension for destination-slot
  lazy migration checks.

Run:

```sh
./run_cacheflex_spm.sh
```

Generated C verifiers and binaries are written under `build/`.

The initial checks focus on SPM-specific safety:

- an L1 in SPM state `X` is not a directory sharer
- an L1 in SPM state `X` is not the directory owner
- directory `I/S/E/M` bookkeeping is internally consistent
- two cores cannot both be stable `E/M` owners of the same line

The physical L1 extension models one set with three ways per core and one
coherent address. Each way carries `{valid, addr, cc_state, is_spm, data}`. Its
`SPMCP_install(dst_way)` rule:

- claims an empty destination way as SPM `X`
- migrates a stable coherent occupant to a free non-SPM way in the same set
- preserves the occupant's address, state, data, and directory metadata
- records rejection when the destination already holds SPM `X`
- records rejection when a coherent occupant has no free non-SPM way
- does not emit Put/Inv/writeback-style coherence side effects
