"""VER-05: DMA agent at the directory (2026-10-11).

Model verification/rumur/v9_dma/private_spm.m = v8 + DMA_READ/DMA_WRITE at the directory
as implemented in MESI_Three_Level_SPM-dir.sm, with fix switches DMA_STALL_SPM,
SWB_IN_DRD (0 implemented / 1 naive stall / 2 accept) and CLEAN_FIX (upstream
MESI_Two_Level-dir CleanReplacement race). DMA_OPS=0 must reproduce VER-04 plain n2_s3
exactly. Implemented-SLICC and naive-stall cases must yield their named counterexamples;
the fixed model must pass; VER-03 mutants must still be caught in the fixed model.
Derived from verification/rumur/v8_finite_lane/run.py, whose docstring follows.

VER-04: finite return-lane extension of the VER-03 model (2026-10-11).

Model verification/rumur/v8_finite_lane/private_spm.m = v7 + FINITE_RETURN_LANE=1, which
lets SPMCP fetches (snapshot capture and S/E/M L0 recall) proceed while L0 returns are
queued, as in the finite profile (separate ordered return lane). Acceptance: legacy
switch (0) reproduces VER-02 v6 n2_s3 exactly; plain finite n2_s3 passes and its
instrumented copy reproduces its counts exactly; other coverage cases show no trace or
violation and errors equal unhit covers; mutants as in VER-03. Derived from
verification/rumur/v7_coverage/run.py (VER-03), whose original docstring follows.

VER-03: Rumur coverage audit and mutation testing of the retained-source model (2026-10-10).

Source model: verification/rumur/v7_coverage/private_spm.m, a copy of the VER-02 model
with six mutation switches (all 0 reproduces VER-02 exactly). Coverage cases instrument
every rule entry and then/else branch with cover statements (instrument.py) and must
reproduce the VER-02 v6 state/rule counts exactly. Mutant cases must yield the named
counterexample; survivors escalate to larger bounds. All case models are generated and
compiled before any checking starts. Same tool, flags, 2-thread affinity, shared broker
reservation and pressure pause as v6 (scripts/run_rumur_refresh_v6.py).
"""
import os, sys, json, time, subprocess, hashlib, re, signal, traceback
from pathlib import Path
ROOT = Path('/data2/rsappidi/cacheflex_mc'); sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'results/parallel_overlap_20260930')]
import broker
from parallel_budget_scheduler import owned
H = ROOT / 'verification/rumur/v9_dma'; V7 = ROOT / 'verification/rumur/v7_coverage'; sys.path.insert(0, str(V7))
import instrument
OUT = ROOT / 'results/rumur_dma_20261011'; V8OUT = ROOT / 'results/rumur_finite_lane_20261011'; GIB = 2 ** 30
TOOL = ROOT / 'results/rumur_refresh_20261001/tools/rumur'; V6 = ROOT / 'results/rumur_refresh_20261001/verification_v6'
TICKET = 'formal_dma_v9'; MODEL = H / 'private_spm.m'


def read(p): return json.loads(Path(p).read_text())
def save(p, x): q = Path(str(p) + '.tmp'); q.write_text(json.dumps(x, indent=2) + '\n'); q.replace(p)
def sha(p):
    h = hashlib.sha256()
    with Path(p).open('rb') as f:
        for x in iter(lambda: f.read(2 ** 20), b''): h.update(x)
    return h.hexdigest()


def v6_counts(name):
    r = read(V6 / name / 'result.json'); assert r['accepted'] and r['outcome'] == 'bounded_pass', name
    return [int(x) for x in r['states'][-1]]


MUTANTS = {  # switch: expected counterexample text (assert message, invariant or liveness name)
    'M_SILENT_REGISTERS': 'silent request changed coherent registration',
    'M_SILENT_STALE_OWNER': 'SPMCP snapshot missed the latest coherent value',
    'M_CAPTURE_FROM_L1': 'SPMCP snapshot missed the latest coherent value',
    'M_EARLY_WB_COMMIT': 'shared copies fresh|SWMR|stale value',
    'M_RETRY_DROPPED': 'all cores can drain',
    'M_INV_KEEPS_COPY': 'shared copies fresh|SWMR|stale value',
}
ESCALATE = [(dict(NCORE=2, STEPS=3, DMA_OPS=1), 2), (dict(NCORE=3, STEPS=3, DMA_OPS=1), 24), (dict(NCORE=2, STEPS=4, DMA_OPS=1), 24)]


FIX = dict(DMA_STALL_SPM=1, SWB_IN_DRD=2, CLEAN_FIX=1)
NOESC = 99


def plan():
    jobs = []
    def add(name, kind, changes, gb, **kw): jobs.append(dict(name=name, kind=kind, changes=changes, gb=gb, **kw))
    add('ctrl_nodma_n2_s3', 'plain', dict(NCORE=2, STEPS=3), 1, reference_v8='plain_n2_s3')
    d1 = dict(NCORE=2, STEPS=3, DMA_OPS=1)
    add('bug_spm_state_dma_n2_s3', 'bug', dict(d1, CLEAN_FIX=1), 1, level=NOESC,
        expected='DMA request in a state with no SLICC transition')
    add('bug_spmwb_in_drd_n2_s3', 'bug', dict(d1, CLEAN_FIX=1, DMA_STALL_SPM=1), 1, level=NOESC,
        expected='request during DMA invalidation has no SLICC transition')
    add('bug_upstream_clean_n2_s3', 'bug', dict(d1, DMA_STALL_SPM=1, SWB_IN_DRD=2), 1, level=NOESC,
        expected='CleanReplacement in a state with no SLICC transition')
    add('naive_stall_n2_s3', 'bug', dict(d1, CLEAN_FIX=1, DMA_STALL_SPM=1, SWB_IN_DRD=1), 2, level=NOESC,
        expected='all cores can drain')
    add('fix_dma1_n2_s2', 'plain', dict(NCORE=2, STEPS=2, DMA_OPS=1, **FIX), 1)
    add('fix_dma1_n2_s3', 'plain', dict(d1, **FIX), 2)
    add('fix_dma1_no_retention_n2_s3', 'plain', dict(d1, RETAIN_LLC=0, **FIX), 2)
    for m in MUTANTS:
        add(f'mut_{m.lower()}_dma1_n2_s3', 'mutant', dict(d1, **FIX, **{m: 1}), 2, mutant=m, expected=MUTANTS[m], level=0)
    add('cov_fix_dma1_n2_s3', 'coverage', dict(d1, **FIX), 2, reference_job='fix_dma1_n2_s3')
    add('fix_dma2_n2_s3', 'plain', dict(NCORE=2, STEPS=3, DMA_OPS=2, **FIX), 24)
    add('fix_dma1_n2_s4', 'plain', dict(NCORE=2, STEPS=4, DMA_OPS=1, **FIX), 24)
    add('fix_dma1_n3_s3', 'plain', dict(NCORE=3, STEPS=3, DMA_OPS=1, **FIX), 24)
    return jobs


def model_text(job, covers_out=None):
    s = MODEL.read_text()
    if job['kind'] == 'coverage':
        s, covers = instrument.instrument(s)
        if covers_out: save(covers_out, covers)
    for k, v in job['changes'].items():
        s, n = re.subn(r'(\b' + k + r':\s*)\d+;', lambda m: m[1] + str(v) + ';', s); assert n == 1, (k, n)
    return s


def config(job):
    s = MODEL.read_text(); c = {k: int(v) for k, v in re.findall(r'^\s*([A-Z_][A-Z0-9_]*):\s*(\d+);', s, re.M)}
    c.update(job['changes']); return c


def const_truth(cond, cfg):
    """True/False if every top-level conjunct is a constant comparison, False if any is false, else None."""
    parts = [p.strip().strip('()').strip() for p in cond.split('&')]
    vals = []
    for p in parts:
        m = re.fullmatch(r'([A-Z_][A-Z0-9_]*)\s*(=|!=)\s*(\d+)', p)
        if m and m[1] in cfg: vals.append((cfg[m[1]] == int(m[3])) == (m[2] == '='))
        elif p == 'true': vals.append(True)
        else: vals.append(None)
    if False in vals: return False
    return True if all(v is True for v in vals) else None


def classify(cover, cfg):
    if cover.get('first', '').startswith('error'): return 'model_error_branch'
    if cover['kind'] == 'then' and const_truth(cover['cond'], cfg) is False: return 'config_disabled'
    if cover['kind'] == 'else' and any(const_truth(c, cfg) is True for c in cover['prior']): return 'config_disabled'
    return 'review'


def main():
    OUT.mkdir(exist_ok=False)
    budget = read(ROOT / 'results/parallel_budget_20260929/budget.json'); os.sched_setaffinity(0, budget['cpu_set'][:2])
    state = dict(pid=os.getpid(), stage='starting', accepted=[], failed=[], survivors=[], pending=[], active=None,
                 scope='VER-05: v9 DMA agent; SLICC DMA bugs, naive stall, fixed model, mutants, coverage')
    def status(stage=None):
        if stage: state['stage'] = stage
        state['updated'] = time.time(); save(OUT / 'status.json', state)
    files = [MODEL, V7 / 'instrument.py', V7 / 'private_spm.m', Path(__file__), TOOL, ROOT / 'verification/rumur/current/private_spm.m']
    files += [V6 / 'private_n2_s3' / 'result.json', H.parent / 'v8_finite_lane' / 'private_spm.m', V8OUT / 'plain_n2_s3' / 'result.json']
    frozen = {str(p): sha(p) for p in files}; save(OUT / 'inputs.json', frozen)
    jobs = plan(); save(OUT / 'plan.json', jobs); state['pending'] = [j['name'] for j in jobs]; status()

    def pressure():
        rss = sum(p['rss'] for p in owned().values())
        mem = {x.split(':')[0]: int(x.split()[1]) * 1024 for x in Path('/proc/meminfo').read_text().splitlines()
               if x.startswith(('MemTotal:', 'MemAvailable:'))}
        return rss > budget['ram_limit'] - GIB or mem['MemAvailable'] < 64 * GIB + max(0, budget['ram_limit'] - rss)

    def reserve(gb):
        while True:
            with broker.locked() as b:
                old = b['leases'].get(TICKET, {}).get('bytes', 0)
                if not b['hold_new'] and time.time() - b['heartbeat'] < 20 and broker.total(b) - old + gb * GIB <= b['cap'] and not pressure():
                    b['leases'][TICKET] = dict(bytes=gb * GIB, owner=os.getpid(), family='formal', acquired=time.time()); return
            status('waiting_for_memory_reservation'); time.sleep(5)

    def run(cmd, log, cap, timeout=3600):
        with log.open('w') as f:
            p = subprocess.Popen(['prlimit', f'--as={cap}:', '--', 'nice', '-n', '10', *map(str, cmd)], cwd=ROOT,
                                 stdout=f, stderr=subprocess.STDOUT)
            state['active'] = dict(pid=p.pid, command=list(map(str, cmd)), log=str(log)); status()
            start = time.monotonic(); paused = False
            try:
                while p.poll() is None:
                    hold = pressure()
                    if hold != paused: p.send_signal(signal.SIGSTOP if hold else signal.SIGCONT); paused = hold
                    state['active']['paused'] = paused; status()
                    if time.monotonic() - start > timeout:
                        if paused: p.send_signal(signal.SIGCONT)
                        p.terminate(); p.wait(timeout=10); return 124
                    time.sleep(.5)
                return p.returncode
            finally:
                if p.poll() is None: p.kill(); p.wait()
                state['active'] = None; status()

    def build(job):
        d = OUT / job['name']; d.mkdir()
        (d / 'model.m').write_text(model_text(job, d / 'covers.json' if job['kind'] == 'coverage' else None))
        cap = max(job['gb'], 1) * GIB - 256 * 2 ** 20
        rc = run([TOOL, '--threads', '2', '--deadlock-detection', 'off', '--output', d / 'checker.c', d / 'model.m'], d / 'generate.log', cap)
        assert rc == 0, ('generate', job['name'], rc, (d / 'generate.log').read_text()[-2000:])
        rc = run(['cc', '-std=c11', '-O1', '-pthread', '-mcx16', d / 'checker.c', '-o', d / 'checker', '-latomic'], d / 'compile.log', cap)
        assert rc == 0, ('compile', job['name'], rc, (d / 'compile.log').read_text()[-2000:])
        job['model_sha256'] = sha(d / 'model.m')

    def judge(job, rc, text):
        d = OUT / job['name']; counts = [[int(a), int(b)] for a, b in re.findall(r'(\d+) states, (\d+) rules fired', text)]
        last = counts[-1] if counts else None; trace = 'The following is the error trace' in text
        violated = re.findall(r'(?:invariant|liveness property) "([^"]+)" (?:failed|violated)', text) + \
            re.findall(r'Assertion failed: \S+: (.+)$', text, re.M) + \
            re.findall(r'error trace for the error:\s*\n\s*\n\s*(\S.*)$', text, re.M)
        r = dict(job, returncode=rc, final_counts=last, trace=trace, violated=sorted(set(violated)), path=str(d))
        if job['kind'] in ('control', 'coverage', 'plain'):
            ref = read(V8OUT / job['reference_v8'] / 'result.json')['final_counts'] if 'reference_v8' in job else \
                v6_counts(job['reference']) if 'reference' in job else (
                read(OUT / job['reference_job'] / 'result.json')['final_counts'] if 'reference_job' in job else None)
            r['reference_counts'] = ref
            ok = last is not None and (ref is None or last == ref) and not trace and 'violated' not in text
            if job['kind'] in ('control', 'plain'):
                ok = ok and rc == 0 and 'No error found.' in text
            else:
                covers = read(d / 'covers.json'); cfg = config(job)
                hits = {l: (int(n) if n else 0) for l, n in re.findall(r'cover "([^"]+)" (?:hit (\d+) times|not hit)', text)}
                errs = re.search(r'(\d+) error\(s\) found', text); unhit = [c for c in covers if hits.get(c['label'], 0) == 0]
                nerr = int(errs[1]) if errs else (0 if 'No error found.' in text else -1)
                ok = ok and len(hits) == len(covers) and nerr == len(unhit)
                for c in covers: c['hits'] = hits.get(c['label']); c['class'] = None if c['hits'] else classify(c, cfg)
                save(d / 'coverage.json', covers)
                r['coverage'] = dict(total=len(covers), hit=len(covers) - len(unhit), unhit=len(unhit),
                                     rules_hit=sum(1 for c in covers if c['kind'] == 'rule' and c['hits']),
                                     rules=sum(1 for c in covers if c['kind'] == 'rule'),
                                     unhit_by_class={k: sum(1 for c in unhit if c['class'] == k) for k in
                                                     ('model_error_branch', 'config_disabled', 'review')})
            r['outcome'] = ('equivalent_pass' if ref is not None else 'bounded_pass') if ok else 'unaccepted'
        else:
            caught = rc != 0 and re.search(job['expected'], text) is not None and (trace or ('liveness property' in text and 'violated' in text))
            survived = rc == 0 and 'No error found.' in text
            r['outcome'] = 'mutant_caught' if caught else ('mutant_survived' if survived else 'unaccepted')
            ok = caught or survived
        r['accepted'] = bool(ok); save(d / 'result.json', r); return r

    try:
        status('precompile')
        for job in jobs:
            reserve(1); build(job)
        save(OUT / 'plan.json', jobs)
        queue = list(jobs)
        while queue:
            job = queue.pop(0)
            if not (OUT / job['name'] / 'checker').exists():
                reserve(1); build(job)
            reserve(job['gb']); state['pending'] = [j['name'] for j in queue]; status(job['name'])
            for p, h in frozen.items(): assert sha(p) == h, ('changed input', p)
            d = OUT / job['name']; rc = run([d / 'checker'], d / 'run.log', job['gb'] * GIB - 256 * 2 ** 20, timeout=24 * 3600)
            r = judge(job, rc, (d / 'run.log').read_text())
            if r['outcome'] == 'mutant_survived':
                state['survivors'].append(job['name'])
                if job['level'] + 1 < len(ESCALATE):
                    ch, gb = ESCALATE[job['level'] + 1]; tag = f"n{ch['NCORE']}_s{ch['STEPS']}"
                    nj = dict(job, name=f"mut_{job['mutant'].lower()}_{tag}", changes=dict(job['changes'], **ch), gb=gb, level=job['level'] + 1)
                    nj.pop('model_sha256', None); queue.insert(0, nj)
            state['accepted' if r['accepted'] else 'failed'].append({k: r[k] for k in r if k not in ('changes',)} | dict(changes=job['changes']))
            status()
            with broker.locked() as b: b['leases'][TICKET]['bytes'] = GIB
        status('complete_requires_review' if not state['failed'] else 'complete_with_failures')
    except BaseException as e:
        state['error'] = repr(e); state['traceback'] = traceback.format_exc(); status('failed'); raise
    finally:
        with broker.locked() as b: b['leases'].pop(TICKET, None)


if __name__ == '__main__':
    main()
