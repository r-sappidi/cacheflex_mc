# CacheFlex MC Agent Notes

## Current Direction

Keep SPM work on `MESI_Three_Level_SPM`. Do not reintroduce the old
two-level SPM protocol path.

## Current GEMM Path

The valid GEMM comparison is the ACL-style FP16 multicore benchmark in
`benchmarks/gemm/`:

- Baseline: `gemm_acl_fp16_blk_mc.cpp`
- CacheFlex: `gemm_acl_fp16_spm_mc.cpp`
- Build: `benchmarks/gemm/build_acl_fp16_mc.sh`
- Coarse sweep: `benchmarks/gemm/run_acl_fp16_w1w7_sweep.sh`
- Focused sweep: `benchmarks/gemm/run_acl_fp16_w1w7_secondary_sweep.sh`

The removed prototype GEMM files used scalar/FP32/generated-tile kernels and
are no longer valid for paper-ready comparisons.

The committed compact result package is:

```text
results/gemm/acl_fp16_w1w7_final_summary/
```

Raw gem5 run directories are intentionally not tracked.

## Useful Commands

Build gem5:

```bash
cd gem5
scons build/ARM_MESI_Three_Level_SPM/gem5.opt -j4
```

Build GEMM:

```bash
source setup_spm_env.sh
bash benchmarks/gemm/build_acl_fp16_mc.sh
```

Run focused W1-W7 sweep:

```bash
bash benchmarks/gemm/run_acl_fp16_w1w7_secondary_sweep.sh
```
