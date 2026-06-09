#!/usr/bin/env python3
import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUMMARY = ROOT / "results/traffic_matrix/summary.csv"
OUT_MD = ROOT / "results/traffic_matrix/vspm_gemm_report.md"
OUT_CSV = ROOT / "results/traffic_matrix/vspm_gemm_report.csv"


def pct(delta, base):
    return 100.0 * delta / base if base else 0.0


def main():
    with SUMMARY.open(newline="") as summary_file:
        rows = list(csv.DictReader(summary_file))

    by_key = {(r["kernel"], int(r["cores"]), r["mode"]): r for r in rows}
    report_rows = []
    for cores in (4, 8):
        private = by_key[("gemm", cores, "private")]
        shared = by_key[("gemm", cores, "shared")]
        private_packets = int(private["generator_packets"])
        shared_packets = int(shared["generator_packets"])
        private_bytes = int(private["network_bytes"])
        shared_bytes = int(shared["network_bytes"])
        private_getx = int(private["l2_getx"])
        shared_getx = int(shared["l2_getx"])
        private_l1_misses = int(private["l1_misses"])
        shared_l1_misses = int(shared["l1_misses"])
        private_l3_stalls = int(private["l3_stalls"])
        shared_l3_stalls = int(shared["l3_stalls"])

        private_bpp = private_bytes / private_packets
        shared_bpp = shared_bytes / shared_packets
        row = {
            "cores": cores,
            "coherent_shared_network_bytes": shared_bytes,
            "vspm_like_private_network_bytes": private_bytes,
            "bytes_removed": shared_bytes - private_bytes,
            "bytes_removed_pct_of_shared": pct(
                shared_bytes - private_bytes, shared_bytes
            ),
            "shared_bytes_per_packet": shared_bpp,
            "vspm_bytes_per_packet": private_bpp,
            "bytes_per_packet_reduction_pct": pct(
                shared_bpp - private_bpp, shared_bpp
            ),
            "shared_l2_getx": shared_getx,
            "vspm_l2_getx": private_getx,
            "getx_removed_pct": pct(shared_getx - private_getx, shared_getx),
            "shared_l1_misses": shared_l1_misses,
            "vspm_l1_misses": private_l1_misses,
            "l1_misses_removed_pct": pct(
                shared_l1_misses - private_l1_misses, shared_l1_misses
            ),
            "shared_l3_stall_events": shared_l3_stalls,
            "vspm_l3_stall_events": private_l3_stalls,
            "l3_stall_events_removed": shared_l3_stalls - private_l3_stalls,
            "fixed_window_ticks": int(shared["sim_ticks"]),
        }
        report_rows.append(row)

    with OUT_CSV.open("w", newline="") as out_file:
        writer = csv.DictWriter(out_file, fieldnames=list(report_rows[0].keys()))
        writer.writeheader()
        writer.writerows(report_rows)

    lines = [
        "# GEMM Coherence Traffic vs VSPM-Like Placement",
        "",
        "Method: compare `gemm shared` against `gemm private` from "
        "`scripts/run_traffic_matrix.py`.",
        "The private case is used as a virtual-scratchpad-memory proxy: "
        "each core's hot data is partitioned so it avoids inter-core "
        "coherent sharing of the working set.",
        "",
        "Important caveat: this is a fixed-time traffic-generator experiment, "
        "not an instruction-level GEMM wall-clock result. `simTicks` is fixed "
        "at 203000 ticks, so the defensible metrics are coherence traffic, "
        "miss/stall events, and NoC bytes per useful generated packet. It "
        "does not directly measure end-to-end GEMM cycles.",
        "",
        "| cores | shared bytes | VSPM-like bytes | bytes removed | "
        "bytes/packet reduction | GETX removed | L1 misses removed | "
        "L3 stall events removed |",
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in report_rows:
        lines.append(
            "| {cores} | {coherent_shared_network_bytes} | "
            "{vspm_like_private_network_bytes} | "
            "{bytes_removed} ({bytes_removed_pct_of_shared:.1f}%) | "
            "{bytes_per_packet_reduction_pct:.1f}% | "
            "{getx_removed_pct:.1f}% | {l1_misses_removed_pct:.1f}% | "
            "{l3_stall_events_removed} |".format(**row)
        )
    lines.extend(
        [
            "",
            "Interpretation: removing hot GEMM data from the coherent domain "
            "eliminates the shared-case L3 stall events in this run and "
            "substantially reduces GETX traffic. The 8-core case is the "
            "stronger scalability signal: shared coherent placement uses "
            "28.2 KB of NoC traffic versus 10.84 KB for the VSPM-like "
            "placement, and the per-useful-packet NoC cost drops by 32.7%.",
        ]
    )
    OUT_MD.write_text("\n".join(lines) + "\n")
    print(OUT_MD)
    print(OUT_CSV)


if __name__ == "__main__":
    main()
