# SPM Multicore Smoke Benchmark

This directory is a small port of the reusable SPM toolchain flow from
`../cacheflex_micro`, adapted for this repo's `MESI_Three_Level_SPM` Ruby work.

It intentionally avoids the old single-core cache-model assumptions. The run
script launches one SE process per simulated core. Each process copies normal
memory lines into private encoded SPM slots with `SPMCP_64_IMM`, uses `dsb sy`,
and consumes the slots with SVE `spm.ld1d`.

Build:

```bash
source ./setup_spm_env.sh
bash benchmarks/spm_smoke/build.sh
```

Run 2 cores, with the default base SPM set of 128 and a 16-way private L1D:

```bash
bash benchmarks/spm_smoke/run_mesi3_spm.sh 2 100 32 128
```

Before this can run, build:

```bash
cd gem5
scons build/ARM_MESI_Three_Level_SPM/gem5.opt -j$(nproc)
cd util/m5
scons build/arm64/abi/arm64/m5op.o arm64.CROSS_COMPILE=aarch64-none-linux-gnu-
mkdir -p build/arm64/out
cp build/arm64/abi/arm64/m5op.o build/arm64/out/
```
