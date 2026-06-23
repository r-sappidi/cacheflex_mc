# ACL FP16 Multicore GEMM Benchmark

This directory contains the current CacheFlex GEMM evaluation path used for the
8-core VL16 W1-W7 results. The benchmark follows the single-core CacheFlex
paper setup as closely as the multicore extension allows: FP16 operands,
ACL-style 8 x 3VL microkernel structure, SVE VL=16, and best-to-best baseline
versus SPM tiling.

## Current Files

- `gemm_acl_fp16_blk_mc.cpp`: coherent-cache baseline.
- `gemm_acl_fp16_spm_mc.cpp`: CacheFlex SPM variant that stages B panels.
- `build_acl_fp16_mc.sh`: builds both gem5 binaries.
- `build.sh`: compatibility wrapper for `build_acl_fp16_mc.sh`.
- `run_acl_fp16_w1w7_sweep.sh`: coarse W1-W7 sweep.
- `run_acl_fp16_w1w7_secondary_sweep.sh`: focused secondary sweep used for the
  final best-to-best results.

Old scalar/FP32/generated-tile prototype kernels were removed to avoid mixing
invalid benchmark paths with the current paper comparison.

## Build

```bash
source setup_spm_env.sh
bash benchmarks/gemm/build_acl_fp16_mc.sh
```

The SPM binary is assembled through `spm_tools/spm_compiler.py` to encode the
CacheFlex SPM pseudo-instructions.

## Run

```bash
# Coarse sweep
bash benchmarks/gemm/run_acl_fp16_w1w7_sweep.sh

# Focused secondary sweep
bash benchmarks/gemm/run_acl_fp16_w1w7_secondary_sweep.sh
```

The compact committed result summary is in
`results/gemm/acl_fp16_w1w7_final_summary/`.
