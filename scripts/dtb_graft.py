"""Graft top-level nodes from one decompiled DTS into another, for bisecting what LK needs.

usage: python3 dtb_graft.py <base.dts> <donor.dts> <out.dts> node [node ...]
Each named top-level node in base is replaced by the donor's version (or added if missing).
Use "-name" to delete a top-level node from base.
"""
import re
import sys


def top_nodes(text):
    """Return {name: (start, end)} for nodes directly under '/ {' in dtc -O dts output."""
    root = text.index('/ {')
    i = text.index('\n', root) + 1
    nodes, depth, start, name = {}, 1, None, None
    for m in re.finditer(r'\{|\};', text[i:]):
        pos = i + m.start()
        if m.group() == '{':
            if depth == 1:
                line_start = text.rfind('\n', 0, pos) + 1
                start, name = line_start, text[line_start:pos].strip().split(':')[-1].strip()
            depth += 1
        else:
            depth -= 1
            if depth == 1 and start is not None:
                nodes[name] = (start, pos + 3)
                start = None
            if depth == 0:
                break
    return nodes


base, donor, out, *names = sys.argv[1:]
b = open(base).read()
d = open(donor).read()
for n in names:
    bn = top_nodes(b)
    if n.startswith('-'):
        s, e = bn[n[1:]]
        b = b[:s] + b[e:]
        continue
    dn = top_nodes(d)
    block = d[dn[n][0]:dn[n][1]]
    if n in bn:
        s, e = bn[n]
        b = b[:s] + block + b[e:]
    else:
        close = b.rindex('};')
        b = b[:close] + block + '\n' + b[close:]
open(out, 'w').write(b)
print("grafted:", ' '.join(names))
