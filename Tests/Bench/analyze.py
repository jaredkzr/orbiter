#!/usr/bin/env python3
"""Frame-rate statistics of ORBITER_VK_FRAMETIMES logs (Tests/Bench/run.sh).

  analyze.py file.ft ...                  table: avg, min, p95, p99 fps per file
  analyze.py -p t0:t1 [-p ...] file.ft    the same per sim-time range
  analyze.py -m file.ft                   GPU passes (median/p99 ms), largest first
  analyze.py -w file.ft                   the slowest frames

Lines: frame, sim time [s], wall time since the previous frame [ms], GPU
total [ms], then GPU marks (name=ms). The first SKIP frames (loading) are
left out (and CPU render=, CPU wait=: the client's CPU time and its wait on
the GPU; columns client (less the wait), core (the rest), wait: medians).
avg = frames / wall time; min = the slowest frame; p95/p99 = the
frame rate of the 95th/99th percentile frame time.
"""
import argparse, re

SKIP = 60

def load(path):
    rows = []
    for i, ln in enumerate(open(path)):
        p = ln.split()
        if i < SKIP or len(p) < 4:
            continue
        m = re.search(r"CPU render=([\d.]+) CPU wait=([\d.]+)", ln)
        rows.append((float(p[1]), float(p[2]), float(p[3]), ln, float(m.group(1)) if m else -1, float(m.group(2)) if m else 0))
    return rows

def pct(xs, q):
    s = sorted(xs)
    k = (len(s) - 1) * q
    i = int(k)
    return s[i] if i + 1 >= len(s) else s[i] + (s[i + 1] - s[i]) * (k - i)

def stats(rows):
    dt = [r[1] for r in rows if r[1] > 0]
    gpu = [r[2] for r in rows if r[2] > 0]
    if not dt:
        return None
    return dict(n=len(dt), wall=sum(dt) / 1000, avg=1000 * len(dt) / sum(dt), min=1000 / max(dt),
                p95=1000 / pct(dt, 0.95), p99=1000 / pct(dt, 0.99), med=pct(dt, 0.5),
                gpu_med=pct(gpu, 0.5) if gpu else 0, gpu_p99=pct(gpu, 0.99) if gpu else 0,
                # (CPU: the client's frame less its wait on the GPU, and the core's: the rest)
                cli=pct([r[4] - r[5] for r in rows if r[4] >= 0], 0.5) if any(r[4] >= 0 for r in rows) else -1,
                core=pct([r[1] - r[4] for r in rows if r[4] >= 0 and r[1] > 0], 0.5) if any(r[4] >= 0 for r in rows) else -1,
                wait=pct([r[5] for r in rows if r[4] >= 0], 0.5) if any(r[4] >= 0 for r in rows) else -1)

def row(label, s):
    if not s:
        return f"{label:24s} (no frames)"
    return (f"{label:24s} {s['n']:8d} {s['wall']:7.0f} {s['avg']:7.0f} {s['min']:7.1f} {s['p95']:7.0f} {s['p99']:7.0f}"
            f" {s['med']:8.2f} {s['gpu_med']:8.2f} {s['gpu_p99']:8.2f}"
            + (f" {s['cli']:7.2f} {s['core']:7.2f} {s['wait']:7.2f}" if s['cli'] >= 0 else ""))

def marks(rows):
    m = {}
    for r in rows:
        # (a pass marked more than once a frame, e.g. terrain per planet: summed)
        f = {}
        for k, v in re.findall(r"([A-Za-z][A-Za-z ]*?)=([\d.]+)", " ".join(r[3].split(" CPU render")[0].split()[4:])):
            f[k.strip()] = f.get(k.strip(), 0.0) + float(v)
        for k, v in f.items():
            m.setdefault(k, []).append(v)
    out = sorted(((pct(v, .5), pct(v, .99), k) for k, v in m.items()), reverse=True)
    return "  " + "\n  ".join(f"{k:16s} {a:7.2f} {b:7.2f}" for a, b, k in out[:15])

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('-p', '--phase', action='append', default=[], help='sim-time range t0:t1')
    ap.add_argument('-m', '--marks', action='store_true', help='GPU passes')
    ap.add_argument('-w', '--worst', action='store_true', help='slowest frames')
    a = ap.parse_args()
    print(f"{'':24s} {'frames':>8s} {'wall s':>7s} {'avg':>7s} {'min':>7s} {'p95':>7s} {'p99':>7s} {'med ms':>8s} {'gpu med':>8s} {'gpu p99':>8s} {'client':>7s} {'core':>7s} {'wait':>7s}")
    for path in a.files:
        rows = load(path)
        name = path.split('/')[-1].rsplit('.', 1)[0]
        print(row(name, stats(rows)))
        for ph in a.phase:
            t0, t1 = map(float, ph.split(':'))
            print(row(f"  {ph}", stats([r for r in rows if t0 <= r[0] < t1])))
        if a.marks:
            print("  GPU pass          median     p99 [ms]\n" + marks(rows))
        if a.worst:
            w = sorted(rows, key=lambda r: -r[1])[:8]
            print("  slowest: " + ", ".join(f"{r[1]:.0f} ms at t={r[0]:.0f} s (GPU {r[2]:.1f})" for r in w))
