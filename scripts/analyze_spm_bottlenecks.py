#!/usr/bin/env python3
"""Extract an architectural-bottleneck profile from a gem5 Ruby stats.txt.

Reads the FIRST stats block (the ROI dump when the workload brackets the ROI
with m5_reset_stats/m5_dump_stats) and reports:
  - per-CPU cycles/IPC and pipeline stall accounting
  - LSQ / SPM-LSQ occupancy pressure and load-to-use latency
  - Ruby sequencer latency (hit vs miss) and outstanding-request occupancy
  - protocol activity: top controller events + SPM-specific transitions
  - NoC message classes, bytes, per-link utilization hotspots
  - message-buffer stall time (protocol back-pressure points)
  - DRAM bandwidth and latency

Usage: analyze_spm_bottlenecks.py stats.txt [label]
"""
import re
import sys
from collections import defaultdict


def read_roi(path):
    stats = {}
    started = False
    for line in open(path):
        if "Begin Simulation Statistics" in line:
            if started:
                break
            started = True
            continue
        if "End Simulation Statistics" in line:
            break
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        key, val = parts[0], parts[1]
        try:
            stats[key] = float(val)
        except ValueError:
            pass
    return stats


def g(stats, key, default=0.0):
    return stats.get(key, default)


def cpus(stats):
    ids = set()
    for k in stats:
        m = re.match(r"system\.cpu(\d+)\.", k)
        if m:
            ids.add(int(m.group(1)))
    return sorted(ids)


def section(title):
    print("\n" + "=" * 72)
    print(title)
    print("=" * 72)


def main():
    path = sys.argv[1]
    label = sys.argv[2] if len(sys.argv) > 2 else path
    s = read_roi(path)
    ncpu = cpus(s)
    ticks = g(s, "simTicks")
    tpc = 400.0  # ticks per cycle at 2.5GHz with 1THz tick rate

    section(f"OVERVIEW [{label}]  ROI = {ticks:.0f} ticks "
            f"({ticks / tpc:.0f} cycles)")
    print(f"{'cpu':>4} {'cycles':>12} {'insts':>12} {'IPC':>6} "
          f"{'idle%':>6}")
    for c in ncpu:
        cyc = g(s, f"system.cpu{c}.numCycles")
        insts = g(s, f"system.cpu{c}.commitStats0.numInsts")
        idle = g(s, f"system.cpu{c}.idleCycles")
        print(f"{c:>4} {cyc:>12.0f} {insts:>12.0f} "
              f"{insts / cyc if cyc else 0:>6.2f} "
              f"{100 * idle / cyc if cyc else 0:>6.1f}")

    section("PIPELINE STALLS (per-CPU counts / cycles)")
    cols = [
        ("lsqFull", "iew.lsqFullEvents"),
        ("spmLsqFull", "iew.spmLsqFullEvents"),
        ("iqFull", "iew.iqFullEvents"),
        ("decBlkCyc", "decode.status::Blocked"),
        ("renBlkCyc", "rename.status::Blocked"),
        ("iewBlkCyc", "iew.dispatchStatus::blocked"),
        ("iewUnblk", "iew.dispatchStatus::unblocking"),
        ("sqSquash", "commit.commitSquashedInsts"),
        ("icStallCyc", "fetchStats0.icacheStallCycles"),
    ]
    hdr = "".join(f"{name:>11}" for name, _ in cols)
    print(f"{'cpu':>4}{hdr}")
    for c in ncpu:
        row = "".join(f"{g(s, f'system.cpu{c}.{key}'):>11.0f}"
                      for _, key in cols)
        print(f"{c:>4}{row}")
    for extra in ("ROBFullEvents", "IQFullEvents", "LQFullEvents",
                  "SQFullEvents", "fullRegistersEvents"):
        tot = sum(g(s, f"system.cpu{c}.rename.{extra}") for c in ncpu)
        if tot:
            print(f"  rename.{extra} total: {tot:.0f}")

    section("LOAD-TO-USE LATENCY (cycles) & LSQ PRESSURE")
    print(f"{'cpu':>4} {'lsq.mean':>9} {'lsq.n':>9} {'spm.mean':>9} "
          f"{'spm.n':>9} {'spmBlkByCache':>14} {'memOrderViol':>13}")
    for c in ncpu:
        print(f"{c:>4} "
              f"{g(s, f'system.cpu{c}.lsq0.loadToUse::mean'):>9.1f} "
              f"{g(s, f'system.cpu{c}.lsq0.loadToUse::samples'):>9.0f} "
              f"{g(s, f'system.cpu{c}.spm_lsq0.loadToUse::mean'):>9.1f} "
              f"{g(s, f'system.cpu{c}.spm_lsq0.loadToUse::samples'):>9.0f} "
              f"{g(s, f'system.cpu{c}.spm_lsq0.blockedByCache'):>14.0f} "
              f"{g(s, f'system.cpu{c}.lsq0.memOrderViolation'):>13.0f}")

    section("RUBY SEQUENCER LATENCY (ruby cycles per request)")
    for name in ("m_latencyHistSeqr", "m_hitLatencyHistSeqr",
                 "m_missLatencyHistSeqr", "m_outstandReqHistSeqr"):
        mean = g(s, f"system.ruby.{name}::mean")
        n = g(s, f"system.ruby.{name}::samples")
        print(f"  {name:<24} mean={mean:>9.1f}  samples={n:>10.0f}")

    section("PROTOCOL ACTIVITY (top event totals per controller)")
    for ctrl in ("L0Cache_Controller", "L1Cache_Controller",
                 "L2Cache_Controller", "Directory_Controller"):
        evs = []
        for k, v in s.items():
            m = re.match(rf"system\.ruby\.{ctrl}\.([A-Za-z0-9_]+)::total$", k)
            if m and v:
                evs.append((m.group(1), v))
        evs.sort(key=lambda kv: -kv[1])
        print(f"  -- {ctrl}")
        for name, v in evs[:14]:
            print(f"    {name:<32} {v:>12.0f}")

    section("SPM TRANSITION DETAIL (state.event totals)")
    for k, v in sorted(s.items(), key=lambda kv: -kv[1]):
        m = re.match(r"system\.ruby\.(L[012]Cache_Controller)\."
                     r"([A-Z0-9_]+)\.([A-Za-z0-9_]+)::total$", k)
        if not m or not v:
            continue
        state, ev = m.group(2), m.group(3)
        if ("SPM" in ev or "Silent" in ev or "SPM" in state
                or state in ("X", "XWB", "IX_D", "SX_L0", "EX_L0",
                             "MX_L0", "MT_SPMS")):
            print(f"    {m.group(1):<22} {state:<10} {ev:<24} {v:>10.0f}")

    section("NOC TRAFFIC")
    tot_msg = tot_byte = 0.0
    for k, v in sorted(s.items()):
        m = re.match(r"system\.ruby\.network\.msg_count\.(\w+)$", k)
        if m:
            b = g(s, f"system.ruby.network.msg_byte.{m.group(1)}")
            tot_msg += v
            tot_byte += b
            print(f"    {m.group(1):<24} msgs={v:>10.0f}  bytes={b:>12.0f}")
    cyc = ticks / tpc if ticks else 1
    print(f"    TOTAL msgs={tot_msg:.0f} bytes={tot_byte:.0f} "
          f"({tot_byte / cyc:.2f} B/cycle across NoC)")
    utils = [(k, v) for k, v in s.items() if "link_utilization" in k]
    if utils:
        utils.sort(key=lambda kv: -kv[1])
        print("    hottest links (accumulated flit-cycles):")
        for k, v in utils[:8]:
            print(f"      {k.replace('system.ruby.network.', ''):<48} "
                  f"{v:>7.2f}")
        vals = [v for _, v in utils]
        print(f"      avg over {len(vals)} links: "
              f"{sum(vals) / len(vals):.2f}")

    section("MESSAGE-BUFFER BACK-PRESSURE (top stall time, ticks)")
    stalls = [(k, v) for k, v in s.items()
              if k.endswith(".m_stall_time") and v > 0]
    stalls.sort(key=lambda kv: -kv[1])
    for k, v in stalls[:14]:
        base = k[:-len(".m_stall_time")]
        cnt = g(s, base + ".m_stall_count")
        print(f"    {base.replace('system.ruby.', ''):<52} "
              f"{v:>12.0f} (stalled msgs {cnt:.0f})")
    if not stalls:
        print("    (none)")

    section("DRAM")
    reads = writes = rbytes = 0.0
    lat_n = lat_t = 0.0
    for k, v in s.items():
        if re.match(r"system\.mem_ctrls\d+\.readBursts", k):
            reads += v
        if re.match(r"system\.mem_ctrls\d+\.writeBursts", k):
            writes += v
        if re.match(r"system\.mem_ctrls\d+\.bytesRead::total", k):
            rbytes += v
        m = re.match(r"system\.mem_ctrls\d+\.requestorReadTotalLat", k)
        if m:
            lat_t += v
    for k, v in s.items():
        if re.match(r"system\.mem_ctrls\d+\.requestorReadAccesses", k):
            lat_n += v
    print(f"    read bursts {reads:.0f}  write bursts {writes:.0f}  "
          f"avg read lat {lat_t / lat_n / tpc if lat_n else 0:.0f} cycles  "
          f"read BW {rbytes / (ticks / 1e12) / 1e9 if ticks else 0:.2f} GB/s")


if __name__ == "__main__":
    main()
