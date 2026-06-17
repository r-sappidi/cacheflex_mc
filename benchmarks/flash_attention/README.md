# Tiled FlashAttention Microbenchmark

This is a standalone pthread benchmark for baseline multicore gem5 runs. It
implements streaming, numerically stable attention over Q/K/V tensors without
materializing the full attention matrix.

The important comparison knobs are explicit:

```text
--threads N
--heads N
--seq-len N
--head-dim N
--q-tile N
--kv-tile N
```

Build for AArch64 SVE:

```bash
bash benchmarks/flash_attention/build.sh
```

Example workload:

```bash
benchmarks/flash_attention/bin_gem5/flash_attention_tiled \
  --threads 4 --heads 8 --seq-len 512 --head-dim 64 \
  --q-tile 16 --kv-tile 64 --warmup 1 --repeat 3 --pin 1
```

The binary calls `m5_reset_stats`, `m5_work_begin`, `m5_work_end`, and
`m5_dump_stats` around only the measured repetitions when compiled with
`-DGEM5`.

For a CacheFlex comparison, keep `heads`, `seq-len`, `head-dim`, `q-tile`,
`kv-tile`, `threads`, and `repeat` identical between baseline and SPM runs.
