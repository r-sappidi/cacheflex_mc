"""VER-06: runtime check of the VER-05 directory DMA fix in gem5.

Same 2-core retained-protocol configuration (gate flags from the accepted
retained spread_roundtrip gate; Mesh_XY because KVFixedMesh rejects DMA; small
LLC so the guest's sweep forces clean replacements) on two binaries:
  old = results/retained_protocol_restart_20261002/build/gem5.opt (unpatched)
  new = results/gem5_dma_fix_20261013/build/gem5.opt (retained + VER-05 patch)
Cases:
  calibrate   old, no DMA, ProtocolTrace to a bounded tick: physical target lines
  ctrl_old/new no DMA: both must PASS with identical stats (patch inert without DMA)
  dma_old_*   DMA on the target lines: expected to hit a missing SLICC transition
  dma_new_*   DMA on the target lines: guest PASS, DMA engines make progress,
              new transitions exercised (per-transition stats)
"""
import os, re, sys, json, time, gzip, hashlib, subprocess
from pathlib import Path
R = Path('/data2/rsappidi/cacheflex_mc'); B = R / 'results/gem5_dma_fix_20261013'; G = 2**30
OLD_BIN = R / 'results/retained_protocol_restart_20261002/build/gem5.opt'; NEW_BIN = B / 'build/gem5.opt'
GATE = R / 'results/retained_protocol_restart_20261002/gates/runs/spread_roundtrip/command.json'
CONFIG = B / 'configs/deprecated/example/dma_race_se.py'; GUEST = B / 'guest/guest'
GUEST_TGT = B / 'guest_targeted/guest'  # -DTARGETED_EVICT: per-line LLC eviction, no sweep
ITERS, LINES, EVICT_KB = 100, 16, 128
sys.path[:0] = [str(R / 'results/parallel_overlap_20260930'), str(R / 'scripts')]
import broker
from parallel_budget_scheduler import owned, memory


def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p, d): Path(p).write_text(json.dumps(d, indent=2) + '\n')


def gate_args():
    c = json.loads(GATE.read_text()); flags, P = [], []; i = 3
    while i < len(c):
        if c[i] == '-P': P.append(c[i + 1]); i += 2; continue
        if c[i] in ('--cmd', '--options'): i += 2; continue
        flags.append(c[i]); i += 1
    swap = {'--topology=KVFixedMesh': '--topology=Mesh_XY', '--mesh-rows=2': '--mesh-rows=1', '--num-cpus=8': '--num-cpus=2',
            '--num-l2caches=8': '--num-l2caches=2', '--num-dirs=8': '--num-dirs=2', '--l2_size=512kB': '--l2_size=32kB', '--l2_assoc=16': '--l2_assoc=4'}
    assert all(k in flags for k in swap), 'gate flags changed'
    flags = [swap.get(f, f) for f in flags]
    keep = lambda p: not re.search(r'(cpu\[|_cntrl)([2-7])\b', p) and 'workload[0].cmd' not in p
    return flags, [p for p in P if keep(p)]


def command(binary, out, dma=(), extra=(), guest=GUEST, evict_kb=EVICT_KB):
    flags, P = gate_args()
    argv = [str(binary), f'--outdir={out}', *extra, str(CONFIG), *flags]
    for p in P: argv += ['-P', p]
    opts = ';'.join(f'{c} {ITERS:03d} {LINES:02d} {evict_kb:03d}' for c in range(2))
    argv += ['--cmd', f'{guest};{guest}', '--options', opts]
    for lo, hi in dma: argv += [f'--dma-range={lo:#x}:{hi:#x}']
    return argv


def launch(name, argv, gb=6, timeout=3 * 3600, log_filter=None):
    out = B / 'runs' / name; out.mkdir(parents=True, exist_ok=False)
    save(out / 'command.json', argv)
    cfg = json.loads((R / 'results/parallel_budget_20260929/budget.json').read_text())
    while True:
        with broker.locked() as b:
            if broker.total(b) + gb * G <= b['cap'] and not b.get('hold_new'):
                b['leases'][f'ver06_{name}'] = dict(bytes=gb * G, owner=os.getpid(), family='VER-06 gem5 DMA'); break
        time.sleep(10)
    try:
        t0 = time.time()
        with (out / 'run.log').open('wb') as f:
            p = subprocess.Popen(['prlimit', f'--as={gb * G}:', '--', *argv], stdout=subprocess.PIPE if log_filter else f,
                                 stderr=subprocess.STDOUT, env=dict(os.environ, LD_LIBRARY_PATH='/data2/rsappidi/conda-envs/cacheflex-gem5/lib'),
                                 preexec_fn=lambda: (os.sched_setaffinity(0, cfg['cpu_set'][:12]), os.nice(10)))
            if log_filter:
                for line in p.stdout:
                    if log_filter(line): f.write(line)
            rc = p.wait(timeout=timeout)
        return dict(name=name, returncode=rc, seconds=round(time.time() - t0, 1), out=str(out))
    finally:
        with broker.locked() as b: b['leases'].pop(f'ver06_{name}', None)


def transitions(out):
    t = {}
    for line in (Path(out) / 'stats.txt').read_text().splitlines() if (Path(out) / 'stats.txt').exists() else []:
        m = re.match(r'system\.ruby\.(Directory_Controller|L2Cache_Controller|DMA_Controller)\.(\w+)\.(\w+)::total\s+(\d+)', line)
        if m: t[f'{m[1]}.{m[2]}.{m[3]}'] = int(m[4])
    return t


def stats_body(out):
    # Simulated statistics only: drop host-time/rate lines.
    keep = [l for l in (Path(out) / 'stats.txt').read_text().splitlines() if not re.match(r'(host|sim_seconds_rate|hostSeconds)', l.split('.')[0] if l else '') and not l.startswith(('host', 'sim_seconds_rate'))]
    return [l for l in keep if not re.match(r'^(hostInstRate|hostOpRate|hostTickRate|hostMemory|hostSeconds)', l)]


DMA = [(0x104000, 0x104400), (0x210000, 0x210400)]


def run_case(name, binary, dma=(), extra_flags=(), gb=6):
    argv = command(binary, B / 'runs' / name, dma=dma)
    argv += list(extra_flags)
    return launch(name, argv, gb=gb)


NEW_TRANSITIONS = ['M_DRD.SPM_Writeback', 'M_DWR.SPM_Writeback', 'M_DRD_SWB.Memory_Ack', 'M_DWR_SWB.Memory_Ack',
                   'M_DRD.CleanReplacement', 'M_DWR.CleanReplacement']
NEW_RECYCLES = [f'{s}.{e}' for s in ('SPM_IS', 'SPM_WB', 'SPM_PUT_WB', 'M_DRD_SWB', 'M_DWR_SWB') for e in ('DMA_READ', 'DMA_WRITE')]
NO_RET = [f'system.ruby.l2_cntrl{i}.spm_retain_llc=False' for i in range(2)]
FAST = ['--dma-min-period=5000', '--dma-max-period=50000']
CASES = {
    'ctrl_old': (OLD_BIN, (), []), 'ctrl_new': (NEW_BIN, (), []),
    'dma_old': (OLD_BIN, DMA, []), 'dma_old_rd': (OLD_BIN, DMA, ['--dma-read-percent=100']),
    'dma_old_wr': (OLD_BIN, DMA, ['--dma-read-percent=0']),
    'dma_new': (NEW_BIN, DMA, []), 'dma_new_rd': (NEW_BIN, DMA, ['--dma-read-percent=100']),
    'dma_new_wr': (NEW_BIN, DMA, ['--dma-read-percent=0']), 'dma_new_fast': (NEW_BIN, DMA, FAST),
    'dma_new_noretain': (NEW_BIN, DMA, [x for p in NO_RET for x in ('-P', p)]),
}
TGT = {  # name: (binary, dma, extra)
    'tgt_ctrl_new': (NEW_BIN, (), []), 'tgt_dma_new': (NEW_BIN, 'TGT', []), 'tgt_dma_new_fast': (NEW_BIN, 'TGT', FAST),
    'tgt_dma_new_rd_fast': (NEW_BIN, 'TGT', FAST + ['--dma-read-percent=100']),
    'tgt_dma_new_wr_fast': (NEW_BIN, 'TGT', FAST + ['--dma-read-percent=0']),
    'tgt_dma_old_fast': (OLD_BIN, 'TGT', FAST),
}


def tgt_ranges():
    r = json.loads((B / 'runs/calibrate_targeted/result.json').read_text())['dma']
    return [tuple(x) for x in r]


def spec(name):
    if name in CASES: return (*CASES[name], GUEST, EVICT_KB)
    binary, dma, extra = TGT[name]
    return binary, tgt_ranges() if dma == 'TGT' else dma, extra, GUEST_TGT, 0


def calibrate(name, guest, evict_kb):
    out = B / 'runs' / name
    if not (out / 'run.log').exists():
        argv = command(OLD_BIN, out, extra=['--debug-flags=ProtocolTrace'], guest=guest, evict_kb=evict_kb) + ['--abs-max-tick=1000000000']
        launch(name, argv, log_filter=lambda l: (b'SPM_Writeback' in l) or not re.match(rb'\s*\d+[: ]', l))
    pages = {}
    for line in (out / 'run.log').read_text().splitlines():
        m = re.search(r'SPM_Writeback\s+\S+ \[0x([0-9a-f]+)', line)
        if m: a = int(m[1], 16); pages.setdefault(a & ~0xfff, set()).add(a & 0xfff)
    assert len(pages) == 2 and all(o == {64 * i for i in range(LINES)} for o in pages.values()), pages
    r = dict(name=name, guest=str(guest), guest_sha256=sha(guest), dma=[(p, p + 64 * LINES) for p in sorted(pages)])
    save(out / 'result.json', r); return r


def evaluate(name):
    binary, dma, extra, guest, evict_kb = spec(name); out = B / 'runs' / name
    log = (out / 'run.log').read_text(errors='replace')
    passes = re.findall(r'SPM_DMA_RACE PASS core=(\d) errors=0 dma_seen_copy=(\d+) dma_seen_load=(\d+)', log)
    panic = re.findall(r'panic: .*\n.*', log); fatal = re.findall(r'fatal: .*', log)
    t = transitions(out); gens = {}
    if (out / 'stats.txt').exists():
        for m in re.finditer(r'system\.dma_gens(\d)\.(numPackets|totalReads|totalWrites|numRetries)\s+(\d+)', (out / 'stats.txt').read_text()):
            gens.setdefault(m[1], {})[m[2]] = int(m[3])
    r = dict(name=name, binary=str(binary), binary_sha256=sha(binary), guest=str(guest), guest_sha256=sha(guest), evict_kb=evict_kb, dma_ranges=[list(x) for x in dma], dma=bool(dma), extra=extra, guest_pass=len(passes) == 2,
             dma_seen=[(int(a), int(b)) for _, a, b in passes], panic=panic[:2], fatal=fatal[:2],
             clean_exit='exiting with last active thread context' in log, dma_gens=gens,
             new_transitions={k: t.get(f'Directory_Controller.{k}', 0) for k in NEW_TRANSITIONS},
             dma_recycles={k: t.get(f'Directory_Controller.{k}', 0) for k in NEW_RECYCLES if t.get(f'Directory_Controller.{k}', 0)},
             dir_dma={k.split('.', 1)[1]: v for k, v in t.items() if k.startswith('Directory_Controller.') and ('DMA' in k or 'M_D' in k or k.endswith(('.ID.Memory_Data', '.ID_W.Memory_Ack')))})
    ok = r['guest_pass'] and r['clean_exit'] and not panic and not fatal
    if dma:
        ok &= all(v.get('numPackets', 0) > 0 and v['numPackets'] - v.get('totalReads', 0) - v.get('totalWrites', 0) <= 64 for v in gens.values()) and len(gens) == 2
        ok &= all(a + b > 0 for a, b in r['dma_seen']) if '--dma-read-percent=100' not in extra else True
    r['outcome'] = ('expected_panic' if panic and binary == OLD_BIN and dma else 'pass' if ok else 'fail')
    save(out / 'result.json', r); return r


GATES = R / 'results/retained_protocol_restart_20261002/gates/runs'
NEW_NAMES = ('M_DRD_SWB', 'M_DWR_SWB')


def stat_map(path):
    d = {}
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith('-') or line.startswith('host') or line.startswith('simSeconds_rate'): continue
        k, _, v = line.partition(' ')
        d[k] = ' '.join(v.split('#')[0].split())
    return d


def gate(name, binary=NEW_BIN, prefix='gate_'):
    out = B / 'runs' / f'{prefix}{name}'
    if not (out / 'run.log').exists():
        argv = json.loads((GATES / name / 'command.json').read_text())
        argv = [str(binary)] + [f'--outdir={out}' if a.startswith('--outdir=') else a for a in argv[1:]]
        launch(f'{prefix}{name}', argv, gb=8)
    ref = json.loads((GATES / name / 'accepted.json').read_text()) if (GATES / name / 'accepted.json').exists() else {}
    markers = ref.get('pass_markers') or json.loads((GATES.parent / 'status.json').read_text()).get('markers', {}).get(name, [])
    log = (out / 'run.log').read_text(errors='replace')
    old, new = stat_map(GATES / name / 'stats.txt'), stat_map(out / 'stats.txt') if (out / 'stats.txt').exists() else {}
    diff = [k for k in old if k in new and old[k] != new[k]]
    missing = [k for k in old if k not in new]
    added = [k for k in new if k not in old]
    def zero(v):
        nums = [x.rstrip('%') for x in v.split() if re.match(r'^-?[0-9.]+%?$', x)]
        return all(float(x) == 0 for x in nums)
    bad_added = [k for k in added if not zero(new[k])]
    r = dict(name=f'{prefix}{name}', binary_sha256=sha(binary), markers=markers, markers_ok=bool(markers) and all(m in log for m in markers),
             equal_stats=len(old) - len(missing) - len(diff), differing=diff[:10], missing=missing[:10], added=len(added),
             added_nonzero=bad_added[:10], added_examples=added[:6])
    r['outcome'] = 'pass' if r['markers_ok'] and not diff and not missing and not bad_added else 'fail'
    save(out / 'result.json', r); return r


if __name__ == '__main__':
    from concurrent.futures import ThreadPoolExecutor
    names = sys.argv[1:]

    def go(n):
        if n.startswith('gate_'): return gate(n[5:])
        if n.startswith('gateold_'): return gate(n[8:], OLD_BIN, 'gateold_')
        if n == 'calibrate': return calibrate(n, GUEST, EVICT_KB)
        if n == 'calibrate_targeted': return calibrate(n, GUEST_TGT, 0)
        binary, dma, extra, guest, evict_kb = spec(n)
        if not (B / 'runs' / n / 'run.log').exists():
            launch(n, command(binary, B / 'runs' / n, dma=dma, guest=guest, evict_kb=evict_kb) + extra)
        return evaluate(n)
    with ThreadPoolExecutor(min(4, len(names))) as ex:
        for r in ex.map(go, names): print(json.dumps({k: r.get(k) for k in ('name', 'outcome', 'dma', 'guest_pass', 'dma_seen', 'panic', 'new_transitions', 'dma_gens', 'equal_stats', 'differing', 'missing', 'added', 'added_nonzero')}))
