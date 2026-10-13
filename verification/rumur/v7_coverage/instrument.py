"""Insert Rumur cover statements into a Murphi model (v7 coverage audit, 2026-10-10).

Rule entry covers ("R|rule") and branch covers ("B|context|L<line>|<n>") after every
`then`/`else` inside rule and procedure bodies. Cover statements add no state, so an
instrumented model must reproduce the uninstrumented state and rule counts exactly.
Functions, startstate and properties are not instrumented. Writes <out>.covers.json
with each cover's condition and first statement for classifying unhit covers.
"""
import json, re, sys
from pathlib import Path

TOK = re.compile(r'"[^"]*"|--.*$|==>|\b(?:then|else|begin|rule|procedure|function|startstate|'
                 r'invariant|liveness|endrule|if|elsif)\b')


def instrument(text):
    lines = text.split('\n'); out = []; covers = []
    ctx = None; kind = None; want_entry = False; cond_start = None; chain = []
    for ln, line in enumerate(lines, 1):
        pieces = []; last = 0
        for m in TOK.finditer(line):
            t = m.group(0)
            if t.startswith('"') or t.startswith('--'):
                continue
            if t == 'rule':
                name = re.match(r'\s*"([^"]*)"', line[m.end():]); ctx = name.group(1) if name else f'rule@{ln}'
                kind = 'rule'; want_entry = False
            elif t == 'procedure':
                name = re.match(r'\s*(\w+)', line[m.end():]); ctx = 'proc:' + name.group(1); kind = 'proc'
            elif t in ('function', 'startstate', 'invariant', 'liveness'):
                kind = None; ctx = None
            elif t == 'endrule':
                kind = None; ctx = None
            elif kind is None:
                continue
            elif t == '==>':
                want_entry = True
            elif t == 'begin' and kind == 'rule' and want_entry:
                want_entry = False; label = f'R|{ctx}'
                covers.append(dict(label=label, kind='rule', context=ctx, line=ln))
                pieces.append((m.end(), f' cover "{label}" true;'))
            elif t in ('if', 'elsif'):
                if t == 'if': chain.append([])
                cond_start = (ln, m.end())
            elif t == 'then':
                cond = text_between(lines, cond_start, (ln, m.start())) if cond_start else '?'
                if chain: chain[-1].append(cond)
                label = f'B|{ctx}|L{ln}|{len(covers)}'
                covers.append(dict(label=label, kind='then', context=ctx, line=ln, cond=cond,
                                   first=first_stmt(lines, ln, m.end())))
                pieces.append((m.end(), f' cover "{label}" true;'))
            elif t == 'else':
                label = f'B|{ctx}|L{ln}|{len(covers)}'
                covers.append(dict(label=label, kind='else', context=ctx, line=ln,
                                   prior=list(chain[-1]) if chain else [], first=first_stmt(lines, ln, m.end())))
                pieces.append((m.end(), f' cover "{label}" true;'))
        # endif closes the innermost chain
        for _ in re.findall(r'\bendif\b', strip(line)):
            if chain: chain.pop()
        for pos, ins in reversed(pieces):
            line = line[:pos] + ins + line[pos:]
        out.append(line)
    return '\n'.join(out), covers


def strip(s):
    return re.sub(r'--.*$', '', re.sub(r'"[^"]*"', '""', s))


def text_between(lines, a, b):
    (l1, c1), (l2, c2) = a, b
    if l1 == l2: s = lines[l1 - 1][c1:c2]
    else: s = lines[l1 - 1][c1:] + ' ' + ' '.join(lines[l1:l2 - 1]) + ' ' + lines[l2 - 1][:c2]
    return ' '.join(strip(s).split())


def first_stmt(lines, ln, col):
    s = lines[ln - 1][col:]
    i = ln
    while not strip(s).strip() and i < len(lines):
        s = lines[i]; i += 1
    return ' '.join(s.split())[:120]


if __name__ == '__main__':
    src, dst = map(Path, sys.argv[1:3])
    text, covers = instrument(src.read_text())
    dst.write_text(text)
    Path(str(dst) + '.covers.json').write_text(json.dumps(covers, indent=1) + '\n')
    print(len(covers), 'covers;', sum(c['kind'] == 'rule' for c in covers), 'rule entries')
