# GEMM Coherence Traffic vs VSPM-Like Placement

Method: compare `gemm shared` against `gemm private` from `scripts/run_traffic_matrix.py`.
The private case is used as a virtual-scratchpad-memory proxy: each core's hot data is partitioned so it avoids inter-core coherent sharing of the working set.

Important caveat: this is a fixed-time traffic-generator experiment, not an instruction-level GEMM wall-clock result. `simTicks` is fixed at 203000 ticks, so the defensible metrics are coherence traffic, miss/stall events, and NoC bytes per useful generated packet. It does not directly measure end-to-end GEMM cycles.

| cores | shared bytes | VSPM-like bytes | bytes removed | bytes/packet reduction | GETX removed | L1 misses removed | L3 stall events removed |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 16632 | 8792 | 7840 (47.1%) | 15.9% | 66.4% | 38.2% | 94 |
| 8 | 28200 | 10840 | 17360 (61.6%) | 32.7% | 81.8% | 44.2% | 338 |

Interpretation: removing hot GEMM data from the coherent domain eliminates the shared-case L3 stall events in this run and substantially reduces GETX traffic. The 8-core case is the stronger scalability signal: shared coherent placement uses 28.2 KB of NoC traffic versus 10.84 KB for the VSPM-like placement, and the per-useful-packet NoC cost drops by 32.7%.
