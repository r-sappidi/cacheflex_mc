# ACL FP16 GEMM W1-W7 Final Summary

Best-to-best multicore GEMM results for the CacheFlex multicore implementation. These are 8-core VL16 / 2048-bit SVE runs with a 6144 GFLOP/s peak denominator. Raw gem5 output directories are intentionally not tracked because they are multi-GB; the CSV records the local raw `stats.txt` provenance paths.

- Geomean SPM speedup: 1.347x
- Aggregate CPU response bytes: baseline 7143.8 MB, SPM 1252.9 MB, ratio 0.175x
- Aggregate Ruby network message bytes: baseline 9080.2 MB, SPM 5936.5 MB, ratio 0.654x

| Shape | Baseline GF/s | SPM GF/s | Speedup | Baseline util | SPM util | Baseline tile | SPM tile |
|---|---:|---:|---:|---:|---:|---|---|
| W1 | 1893.82 | 2438.00 | 1.287x | 30.8% | 39.7% | mc128_kc256_grid8 | mc128_kc128_grid8 |
| W2 | 2192.98 | 2871.31 | 1.309x | 35.7% | 46.7% | mc96_kc64_grid8 | mc96_kc128_grid8 |
| W3 | 2055.94 | 2706.91 | 1.317x | 33.5% | 44.1% | mc96_kc128_grid4 | mc96_kc256_grid4 |
| W4 | 2319.83 | 3964.70 | 1.709x | 37.8% | 64.5% | mc96_kc64_grid2 | mc96_kc256_grid2 |
| W5 | 1369.26 | 1769.97 | 1.293x | 22.3% | 28.8% | mc96_kc128_grid2 | mc96_kc256_grid2 |
| W6 | 1160.33 | 1412.98 | 1.218x | 18.9% | 23.0% | mc96_kc64_grid2 | mc96_kc64_grid2 |
| W7 | 1920.23 | 2582.98 | 1.345x | 31.3% | 42.0% | mc96_kc128_grid2 | mc96_kc128_grid2 |

See `best_to_best_w1_w7_vl16_8core.csv` for traffic counters and raw stats provenance.
