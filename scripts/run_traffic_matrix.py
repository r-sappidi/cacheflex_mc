#!/usr/bin/env python3
import csv
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEM5 = ROOT / "build/ARM_MESI_Three_Level/gem5.opt"
CONFIG = ROOT / "configs/run_mesi3_traffic_model.py"
OUT = ROOT / "results/traffic_matrix"


def stat_first(text, pattern):
    match = re.search(pattern, text, re.MULTILINE)
    return int(float(match.group(1))) if match else 0


def stat_sum(text, pattern):
    return sum(int(float(x)) for x in re.findall(pattern, text, re.MULTILINE))


def collect(outdir):
    text = (outdir / "stats.txt").read_text(errors="ignore")
    control_msgs = stat_first(
        text, r"network\.msg_count\.Control\s+([0-9.eE+-]+)"
    )
    response_msgs = stat_first(
        text, r"network\.msg_count\.Response_Data\s+([0-9.eE+-]+)"
    )
    control_bytes = stat_first(
        text, r"network\.msg_byte\.Control\s+([0-9.eE+-]+)"
    )
    response_bytes = stat_first(
        text, r"network\.msg_byte\.Response_Data\s+([0-9.eE+-]+)"
    )
    return {
        "sim_ticks": stat_first(text, r"^simTicks\s+([0-9.eE+-]+)"),
        "control_msgs": control_msgs,
        "response_data_msgs": response_msgs,
        "network_msgs": control_msgs + response_msgs,
        "control_bytes": control_bytes,
        "response_data_bytes": response_bytes,
        "network_bytes": control_bytes + response_bytes,
        "l2_gets": stat_first(
            text, r"L2Cache_Controller\.L1_GETS::total\s+([0-9.eE+-]+)"
        ),
        "l2_getx": stat_first(
            text, r"L2Cache_Controller\.L1_GETX::total\s+([0-9.eE+-]+)"
        ),
        "l1_misses": stat_sum(
            text, r"l1_controllers\d+\.Dcache\.m_demand_misses\s+([0-9.eE+-]+)"
        ),
        "l3_stalls": stat_sum(
            text, r"L1RequestToL2Cache\.m_stall_count\s+([0-9.eE+-]+)"
        ),
        "generator_packets": stat_sum(
            text, r"processor\.cores\d+\.generator\.numPackets\s+([0-9.eE+-]+)"
        ),
    }


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    jobs = []
    for kernel in ("gemm", "flash"):
        for cores in (4, 8):
            for mode in ("private", "shared"):
                jobs.append((kernel, mode, cores))

    for kernel, mode, cores in jobs:
        name = f"{kernel}_{mode}_{cores}c"
        outdir = OUT / name
        cmd = [
            str(GEM5),
            "-d",
            str(outdir),
            str(CONFIG),
            "--kernel",
            kernel,
            "--mode",
            mode,
            "--cores",
            str(cores),
            "--duration",
            "200ns",
            "--data-limit",
            "8192",
        ]
        print("RUN", name, flush=True)
        subprocess.run(cmd, cwd=ROOT, check=True)
        row = {"run": name, "kernel": kernel, "mode": mode, "cores": cores}
        row.update(collect(outdir))
        rows.append(row)

    by_key = {(r["kernel"], r["cores"], r["mode"]): r for r in rows}
    for row in rows:
        other = by_key.get((row["kernel"], row["cores"], "private"))
        if row["mode"] == "shared" and other:
            row["shared_vs_private_network_bytes"] = round(
                row["network_bytes"] / max(1, other["network_bytes"]), 3
            )
            row["shared_vs_private_l3_stalls"] = round(
                row["l3_stalls"] / max(1, other["l3_stalls"]), 3
            )
        else:
            row["shared_vs_private_network_bytes"] = ""
            row["shared_vs_private_l3_stalls"] = ""

    with (OUT / "summary.csv").open("w", newline="") as out_file:
        writer = csv.DictWriter(out_file, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print((OUT / "summary.csv").read_text())


if __name__ == "__main__":
    main()
