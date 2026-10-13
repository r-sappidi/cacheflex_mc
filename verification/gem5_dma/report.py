"""VER-06 status.json and REPORT.md from per-run result.json files."""
import json, time
from pathlib import Path
import run

GUEST_CASES = list(run.CASES) + list(run.TGT)
EXPECT = {n: ('expected_panic' if b == run.OLD_BIN and d else 'pass') for n, (b, d, e) in {**run.CASES, **run.TGT}.items()}


def pkts(g):
    return ', '.join('%d/%d' % (v['numPackets'], v.get('totalReads', 0) + v.get('totalWrites', 0)) for v in g.values()) or '-'


def main():
    B = run.B; rows = []
    cal = {n: json.loads((B / 'runs' / n / 'result.json').read_text()) for n in ('calibrate', 'calibrate_targeted')}
    assert [tuple(x) for x in cal['calibrate']['dma']] == run.DMA, 'non-targeted DMA ranges differ from calibration'
    for n in GUEST_CASES:
        r = json.loads((B / 'runs' / n / 'result.json').read_text())
        rows.append(dict(r, kind='guest', expected=EXPECT[n], accepted=r['outcome'] == EXPECT[n], path=str(B / 'runs' / n)))
    gates = sorted(p.name for p in (B / 'runs').iterdir() if p.name.startswith(('gate_', 'gateold_')))
    for n in gates:
        r = json.loads((B / 'runs' / n / 'result.json').read_text())
        rows.append(dict(r, kind='gate_replay', expected='pass', accepted=r['outcome'] == 'pass', path=str(B / 'runs' / n)))
    old, new = run.stat_map(B / 'runs/ctrl_old/stats.txt'), run.stat_map(B / 'runs/ctrl_new/stats.txt')
    ctrl = dict(name='ctrl_old_vs_new_stats', kind='equivalence', equal=sum(old[k] == new.get(k) for k in old),
                differing=[k for k in old if k in new and old[k] != new[k]][:10], missing=[k for k in old if k not in new][:10],
                added=[k for k in new if k not in old][:10])
    ctrl['accepted'] = not ctrl['differing'] and not ctrl['missing']
    rows.append(ctrl)
    acc = [r for r in rows if r['accepted']]; bad = [r for r in rows if not r['accepted']]
    stage = 'complete_requires_review' if not bad else 'complete_with_failures'
    run.save(B / 'status.json', dict(stage=stage, scope='VER-06: gem5 runtime check of the VER-05 directory DMA fix', accepted=acc,
                                     failed=bad, pending=[], active=None, updated=time.time(),
                                     binaries=dict(old=run.sha(run.OLD_BIN), new=run.sha(run.NEW_BIN))))
    L = ['# VER-06: gem5 runtime check of the VER-05 directory DMA fix', '', f'Stage: {stage}; {len(acc)}/{len(rows)} accepted.', '',
         f'- old binary (retained, unpatched): `{run.OLD_BIN}` sha256 {run.sha(run.OLD_BIN)[:16]}',
         f'- new binary (retained + patch): `{run.NEW_BIN}` sha256 {run.sha(run.NEW_BIN)[:16]}',
         f"- DMA target pages (calibrated): {', '.join(hex(a) for a, _ in run.DMA)} (16 lines each)", '',
         '## Guest cases', '', '| Case | Binary | DMA | Expected | Outcome | Guest | DMA seen (copy,load) per core | DMA pkts sent/completed | '
         'M_DRD/M_DWR.SPM_Writeback | *_SWB.Memory_Ack | M_DRD/M_DWR.CleanReplacement | Panic |', '|' + '---|' * 12]
    for r in rows:
        if r['kind'] != 'guest': continue
        t = r['new_transitions']; g = r['dma_gens']
        L.append(f"| {r['name']} | {'new' if r['binary'] == str(run.NEW_BIN) else 'old'} | {'yes' if r['dma_ranges'] else 'no'} {' '.join(r['extra']) if r['extra'] and r['extra'][0] != '-P' else ('no-retain' if r['extra'] else '')} | "
                 f"{r['expected']} | {r['outcome']} | {'PASS' if r['guest_pass'] else '-'} | {r['dma_seen']} | "
                 f"{pkts(g)} | "
                 f"{t['M_DRD.SPM_Writeback']}/{t['M_DWR.SPM_Writeback']} | {t['M_DRD_SWB.Memory_Ack']}/{t['M_DWR_SWB.Memory_Ack']} | "
                 f"{t['M_DRD.CleanReplacement']}/{t['M_DWR.CleanReplacement']} | {r['panic'][0].splitlines()[-1] if r['panic'] else '-'} |")
    L += ['', f"No-DMA control, old vs new binary: {ctrl['equal']} simulated stats equal, {len(ctrl['differing'])} differing, "
          f"{len(ctrl['missing'])} missing, {len(ctrl['added'])} added -> {'identical' if ctrl['accepted'] else 'DIFFERENT'}", '',
          '## DMA requests recycled in SPM states (new binary)', '']
    for r in rows:
        if r.get('kind') == 'guest' and r['binary'] == str(run.NEW_BIN) and r['dma_recycles']:
            L.append(f"- {r['name']}: " + ', '.join(f'{k} {v}' for k, v in r['dma_recycles'].items()))
    L += ['', '## Retained runtime gates replayed on the new binary', '', '| Gate | Outcome | Pass markers | Equal simulated stats | Differing | Missing | Added (all zero) |', '|---|---|---|---|---|---|---|']
    for r in rows:
        if r['kind'] == 'gate_replay':
            L.append(f"| {r['name']} | {r['outcome']} | {r['markers_ok']} | {r['equal_stats']} | {len(r['differing'])} | {len(r['missing'])} | {r['added']} ({not r['added_nonzero']}) |")
    (B / 'REPORT.md').write_text('\n'.join(L) + '\n'); print('\n'.join(L))


if __name__ == '__main__':
    main()
