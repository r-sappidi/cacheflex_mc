"""VER-03 report: per-case and union coverage, unhit covers by class, mutant outcomes."""
import json
from pathlib import Path
OUT = Path('/data2/rsappidi/cacheflex_mc/results/rumur_coverage_20261010')
RETAINED = ['cov_n2_s2', 'cov_n2_s3', 'cov_n2_s4', 'cov_n3_s3', 'cov_n2_s5']


def main():
    s = json.loads((OUT / 'status.json').read_text()); rows = {r['name']: r for r in s['accepted'] + s['failed']}
    L = ['# VER-03 coverage and mutation report', '', f"Queue stage: {s['stage']}", '',
         '| Case | Outcome | States / rules | v6 reference | Covers hit | Rules hit | Unhit: error / config / review |',
         '|---|---|---|---|---|---|---|']
    for n, r in rows.items():
        if r['kind'] == 'mutant': continue
        c = r.get('coverage') or {}; u = c.get('unhit_by_class', {})
        L.append(f"| {n} | {r['outcome']} | {r['final_counts']} | {r.get('reference_counts')} | "
                 f"{c.get('hit', '-')}/{c.get('total', '-')} | {c.get('rules_hit', '-')}/{c.get('rules', '-')} | "
                 f"{u.get('model_error_branch', '-')} / {u.get('config_disabled', '-')} / {u.get('review', '-')} |")
    L += ['', '## Mutants', '', '| Case | Outcome | Detected by | States at detection |', '|---|---|---|---|']
    for n, r in rows.items():
        if r['kind'] == 'mutant':
            v = [x for x in r.get('violated', []) if not x.startswith('Assertion failed')]
            L.append(f"| {n} | {r['outcome']} | {'; '.join(v) or '-'} | {r['final_counts']} |")
    cov = {n: json.loads((OUT / n / 'coverage.json').read_text()) for n in RETAINED if (OUT / n / 'coverage.json').exists()}
    if cov:
        base = next(iter(cov.values())); union = []
        for i, c in enumerate(base):
            hits = {n: v[i]['hits'] or 0 for n, v in cov.items()}
            first = next((n for n in RETAINED if hits.get(n)), None)
            union.append(dict(c, hits_by_case=hits, first_bound=first, cls=None if first else c['class']))
        unhit = [u for u in union if not u['first_bound']]
        L += ['', f'## Union over retained cases ({", ".join(cov)})', '',
              f"Covers hit: {len(union) - len(unhit)}/{len(union)}; rule entries hit: "
              f"{sum(1 for u in union if u['kind'] == 'rule' and u['first_bound'])}/{sum(1 for u in union if u['kind'] == 'rule')}", '']
        L += ['First bound reaching each cover (hit only at larger bounds):', '']
        for n in RETAINED[1:]:
            late = [u for u in union if u['first_bound'] == n]
            if late: L.append(f"- {n}: " + '; '.join(f"{u['context']} L{u['line']} ({u.get('cond', u['kind'])})" for u in late))
        for cls in ('review', 'config_disabled', 'model_error_branch'):
            items = [u for u in unhit if u['cls'] == cls]
            L += ['', f'### Unhit — {cls} ({len(items)})', '']
            for u in items:
                L.append(f"- `{u['context']}` L{u['line']} {u['kind']}: `{u.get('cond') or 'else after ' + ' | '.join(u.get('prior', []))}` -> `{u.get('first', '')}`")
        json.dump(union, (OUT / 'union_coverage.json').open('w'), indent=1)
    (OUT / 'REPORT.md').write_text('\n'.join(L) + '\n'); print('\n'.join(L))


if __name__ == '__main__':
    main()
