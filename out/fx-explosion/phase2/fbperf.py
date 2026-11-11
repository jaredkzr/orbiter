#!/usr/bin/env python3
"""Fireball pass costs (fb grid, fb march, fb comp) per window after the break-up: median / max [ms]."""
import json, re, sys, statistics
WIN = [(0.0, 0.5), (0.5, 1.0), (1.0, 2.0), (2.0, 3.0), (3.0, 3.8)]
for d in sys.argv[1:]:
    tb = json.load(open(d + '/run.json'))['breakup_simt']
    rows = []
    for line in open(d + '/frames.ft'):
        f = line.split(None, 4)
        if len(f) < 5: continue
        p = {k.strip(): float(v) for k, v in re.findall(r"([a-zA-Z][a-zA-Z ]*?)=([0-9.]+)", f[4])}
        rows.append((float(f[1]) - tb, float(f[3]), p))
    print(d)
    print("  window     |  grid med/max |  march med/max |  comp med/max |  fb sum max | frame med")
    for a, b in WIN:
        w = [r for r in rows if a <= r[0] < b]
        if not w: continue
        def mm(k):
            v = [r[2].get(k, 0.0) for r in w]
            return statistics.median(v), max(v)
        g, m, c = mm('fb grid'), mm('fb march'), mm('fb comp')
        s = max(sum(r[2].get(k, 0.0) for k in ('fb grid', 'fb march', 'fb comp')) for r in w)
        print("  %4.1f..%3.1f  |  %.2f / %.2f  |  %.2f / %.2f   |  %.2f / %.2f  |   %.2f     | %.2f" % (a, b, g[0], g[1], m[0], m[1], c[0], c[1], s, statistics.median(r[1] for r in w)))
