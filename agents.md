# CacheFlex MC Agent Notes

## Current Direction

Keep SPM work on `MESI_Three_Level_SPM`. Do not reintroduce the old
two-level SPM protocol path.

## Benchmarks

Performance benchmarks and results have intentionally been removed so the
next evaluation can start from a clean design. Keep correctness-oriented SPM
smoke tests separate from future performance workloads.

## Useful Commands

Build gem5:

```bash
cd gem5
scons build/ARM_MESI_Three_Level_SPM/gem5.opt -j4
```

Build the SPM smoke test:

```bash
source setup_spm_env.sh
bash benchmarks/spm_smoke/build.sh
```
