# Blocked GEMM Microbenchmark (CacheFlex multicore baseline)

Standalone pthread + SVE single-precision GEMM, `C += A * B` in BLIS
convention (`C[MxN] = A[MxK] * B[KxN]`). Defaults are workload **W4**
(BERT-Base attention projection, M=512 K=768 N=768) from the single-core
CacheFlex evaluation. Other paper shapes are reachable through `--m/--k/--n`
(e.g. W6 = `--m 2048 --k 64 --n 2048`, trimmed W3 = `--m 128 --k 2048
--n 2048`).

## Why it is shaped this way

* **M-partitioning:** each thread owns a contiguous block of C/A rows, so A
  and C are thread-private and **B is read-shared by every core**. On the
  stock MESI three-level baseline this replicates the same B panels into
  every core's private L0/L1 and refetches them over the network — the
  multicore overhead the SPM variant is meant to remove.
* **Blocking:** the loop nest is `jc (nc) -> pc (kc) -> i -> j-vector -> p`,
  so one `kc x nc` panel of B stays cache-hot across all rows a thread owns,
  and a C row segment stays in SVE registers across the `kc` accumulation.
  Defaults `kc=128 nc=64` give a 32 KiB B panel (fits the 64 KiB L0-D).
* **ROI hygiene:** workers are spawned once and parked on a barrier;
  `m5_reset_stats`/`m5_work_begin` fire after warmup while workers are
  parked, so thread create/join and tensor init never pollute the ROI. No
  heap allocation happens inside the ROI.
* **Verification:** `--verify N` recomputes N sampled C entries in double
  precision (expected value is `(warmup+repeat) * dot`, since each rep
  accumulates into C). Exit status is nonzero on mismatch.

## Build and run

```bash
source setup_spm_env.sh
bash benchmarks/gemm/build.sh

# W4 on the stock MESI three-level baseline, 8 cores:
bash benchmarks/gemm/run_mesi3_baseline.sh 8

# Strong-scaling sweep for the motivation data:
for c in 1 2 4 8; do bash benchmarks/gemm/run_mesi3_baseline.sh "$c"; done

# Other shapes: run_mesi3_baseline.sh CORES M K N (KC/NC/REPEAT/LABEL via env)
bash benchmarks/gemm/run_mesi3_baseline.sh 8 2048 64 2048   # W6
```

The run script reproduces the system used for the flash-attention baseline
results: 64 kB 4-way private L0 I/D, 512 kB 8-way private L1, one shared
4 MB 16-way L2 bank, 2 directories, DerivO3CPU at 1.5 GHz, SVE VL=4
(512-bit). Pass extra gem5 flags through `GEM5_EXTRA` (e.g. prefetcher
toggles).

For a CacheFlex comparison keep `m/k/n/kc/nc/threads/repeat` identical
between baseline and SPM runs. Note W4's total working set (~5.25 MiB) is
sized against the 4 MiB shared L2: B (2.25 MiB) fits the LLC but not a
512 kB private L1, so the signal to watch across the core sweep is
B-refetch traffic (router link bytes, L0/L1 replacements) growing with core
count while useful FLOPs stay constant.
