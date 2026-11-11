#!/usr/bin/env python3
"""Crash perf summary from an ORBITER_VK_FRAMETIMES file (run.py --ft), re-based on the
break-up (OUTDIR/run.json).

  perf.py OUTDIR [OUTDIR_B]   (two: A vs B side by side)

Per window after the break-up: frame GPU ms median; the particle passes ("pt *",
"particles", "fb *") summed per frame, max in the window; the passes the crash moves
(rtl trace, rt build, ssr, prepass). Then the peak particle frame and the crash
peak (the slowest frame in 0..3 s after the break-up; median of 0..3 s).
"""
import json, os, re, statistics, sys

WIN = [(-1.0, -0.5), (0.0, 0.5), (0.5, 1.0), (1.0, 2.0), (2.0, 3.0), (3.0, 5.0), (5.0, 8.0), (8.0, 12.0), (12.0, 20.0)]
PASS = re.compile(r"([a-zA-Z][a-zA-Z ]*?)=([0-9.]+)")


def load(d):
    rj = json.load(open(os.path.join(d, "run.json")))
    tb = rj["breakup_simt"]
    rows = []
    for line in open(os.path.join(d, "frames.ft")):
        f = line.split(None, 4)
        if len(f) < 5: continue
        simt, gpu = float(f[1]), float(f[3])
        p = {}
        for k, v in PASS.findall(f[4]):
            k = k.strip(); p[k] = p.get(k, 0.0) + float(v)
        # (the remote's screenshot frames read back the image: not in the GPU passes)
        rows.append((simt - tb, gpu, p))
    return rows


def pt(p): return sum(v for k, v in p.items() if k.startswith("pt ") or k == "particles" or k.startswith("fb "))


def summary(rows):
    out = []
    for a, b in WIN:
        w = [r for r in rows if a <= r[0] < b]
        if not w: continue
        med = lambda k: statistics.median(r[2].get(k, 0.0) for r in w)
        out.append((f"{a:+5.1f}..{b:<5.1f}", statistics.median(r[1] for r in w), max(pt(r[2]) for r in w),
                    max(r[2].get("pt sprites", 0.0) for r in w), med("rtl trace"), med("rt build"), med("ssr"), med("prepass")))
    crash = [r for r in rows if 0.0 <= r[0] < 3.0]
    peak = max(crash, key=lambda r: r[1]) if crash else None
    pk = max(rows, key=lambda r: pt(r[2]))
    return out, peak, statistics.median(r[1] for r in crash) if crash else 0.0, pk


def main():
    dirs = sys.argv[1:]
    res = [summary(load(d)) for d in dirs]
    print("GPU ms; t after the break-up [s]; frame = median, pt = particle passes summed (max), sprites (max), others median")
    hdr = f"{'window':12s}" + "".join(f" | {'frame':>6s} {'pt':>5s} {'sprt':>5s} {'rtl':>5s} {'rtb':>5s} {'ssr':>5s} {'pre':>5s}" for _ in dirs)
    print(hdr)
    for i in range(len(res[0][0])):
        line = f"{res[0][0][i][0]:12s}"
        for r in res:
            if i < len(r[0]):
                w = r[0][i]; line += f" | {w[1]:6.2f} {w[2]:5.2f} {w[3]:5.2f} {w[4]:5.2f} {w[5]:5.2f} {w[6]:5.2f} {w[7]:5.2f}"
        print(line)
    for d, (w, peak, med, pk) in zip(dirs, res):
        print(f"{os.path.basename(d.rstrip('/'))}: crash 0..3 s median {med:.2f} ms, slowest {peak[1]:.2f} ms at t {peak[0]:+.2f};"
              f" peak particle frame t {pk[0]:+.2f}: {pt(pk[2]):.2f} ms (" +
              " ".join(f"{k}={v:.2f}" for k, v in sorted(pk[2].items()) if (k.startswith('pt ') or k.startswith('fb ')) and v >= 0.02) + ")")


if __name__ == "__main__":
    main()
