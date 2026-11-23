#!/usr/bin/env python3
"""Frame-rate statistics of ORBITER_VK_FRAMETIMES logs (Tests/Bench/run.sh).

  analyze.py file.ft ...                  table: frame rate and frame times per file
  analyze.py -p t0:t1 [-p ...] file.ft    the same per sim-time range
  analyze.py -m file.ft                   GPU passes (median/p99 ms), largest first
  analyze.py -c file.ft                   CPU sections (median/p99 ms), largest first
  analyze.py -w file.ft                   the slowest frames, with their largest CPU sections
  analyze.py -j out/summary.json f.ft ... a summary per file (and its phases) as JSON,
                                          with the run's stamp (compare.py reads it)

Lines (VulkanClient.cpp clbkDisplayFrame): an optional header "# orbiter-frametimes 2
key=value ..." (device, size, settings, the benchmark's stamp), then per frame: the
frame, sim time [s], wall time since the previous frame [ms], GPU total [ms], the GPU
passes (name=ms), "CPU render=" (the client, from clbkRenderScene to the frame's end),
"CPU wait=" (its wait on the GPU), then in version 2 "@cpu" and the CPU's sections
(name=ms: the core's state update parts "core pre/step/post", the rest of its frame
"core other", "core input", clbkUpdate's, the CPU time up to each GPU pass's mark,
"overlay", "2d ui", "submit") and "@bound=" G or C (the CPU waited on the GPU or not).
The first SKIP frames (loading) are left out.

Table: avg = frames / wall time; p50/p95/p99/max: frame times [ms]; 1% low: the
mean frame rate of the slowest 1% of frames; stutter: frames over twice the median
and over 50 ms; gpu: median of the GPU total; client (less the wait), core, wait:
medians [ms]; Gbound: the share of frames the CPU waited on the GPU.
"""
import argparse, json, os, re, shlex

SKIP = 60
MARK = re.compile(r"([A-Za-z0-9][A-Za-z0-9 ]*?)=([\d.]+)")

def parse_marks(text):
    # (a pass marked more than once a frame, e.g. terrain per planet: summed)
    f = {}
    for k, v in MARK.findall(text):
        f[k.strip()] = f.get(k.strip(), 0.0) + float(v)
    return f

def load(path):
    """(header dict, rows): a row is a dict t, wall, gpu, render, wait, gpum, cpum, bound."""
    head, rows, n = {}, [], 0
    for ln in open(path, errors="replace"):
        if ln.startswith("#"):
            try:
                for tok in shlex.split(ln[1:])[2:]:
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        head[k] = v
            except ValueError:
                pass
            continue
        n += 1
        p = ln.split()
        if n <= SKIP or len(p) < 4:
            continue
        body = " ".join(p[4:])
        gpart, _, rest = body.partition(" CPU render=")
        if not rest and body.startswith("CPU render="):
            gpart, rest = "", body[len("CPU render="):]
        m = re.match(r"([\d.]+) CPU wait=([\d.]+)", rest)
        cpart = rest.split("@cpu", 1)[1].split("@bound", 1)[0] if "@cpu" in rest else ""
        b = re.search(r"@bound=([GC])", rest)
        rows.append(dict(t=float(p[1]), wall=float(p[2]), gpu=float(p[3]),
                         render=float(m.group(1)) if m else -1.0, wait=float(m.group(2)) if m else 0.0,
                         gpum=parse_marks(gpart), cpum=parse_marks(cpart), bound=b.group(1) if b else None))
    return head, rows

def pct(xs, q):
    s = sorted(xs)
    if not s:
        return 0.0
    k = (len(s) - 1) * q
    i = int(k)
    return s[i] if i + 1 >= len(s) else s[i] + (s[i + 1] - s[i]) * (k - i)

def stats(rows):
    dt = [r["wall"] for r in rows if r["wall"] > 0]
    if not dt:
        return None
    gpu = [r["gpu"] for r in rows if r["gpu"] > 0]
    med = pct(dt, 0.5)
    slow = sorted(dt)[-max(1, len(dt) // 100):]
    cpu = [r for r in rows if r["render"] >= 0 and r["wall"] > 0]
    core = [sum(v for k, v in r["cpum"].items() if k.startswith("core")) for r in cpu if r["cpum"]]
    bound = [r["bound"] for r in rows if r["bound"]]
    return dict(frames=len(dt), wall_s=sum(dt) / 1000, fps_avg=1000 * len(dt) / sum(dt),
                ms_p50=med, ms_p95=pct(dt, 0.95), ms_p99=pct(dt, 0.99), ms_max=max(dt),
                fps_1pct_low=1000 * len(slow) / sum(slow),
                stutters=sum(1 for x in dt if x > 2 * med and x > 50),
                gpu_med=pct(gpu, 0.5), gpu_p99=pct(gpu, 0.99),
                # (the client's frame less its wait on the GPU; the core: its measured
                # parts where logged, else the wall time less the client's)
                client_med=pct([r["render"] - r["wait"] for r in cpu], 0.5) if cpu else -1,
                core_med=pct(core, 0.5) if core else (pct([r["wall"] - r["render"] for r in cpu], 0.5) if cpu else -1),
                wait_med=pct([r["wait"] for r in cpu], 0.5) if cpu else -1,
                gpu_bound=sum(1 for b in bound if b == "G") / len(bound) if bound else -1)

HDR = (f"{'':24s} {'frames':>7s} {'avg':>6s} {'1%low':>6s} {'p50':>6s} {'p95':>6s} {'p99':>6s} {'max':>7s}"
       f" {'stut':>4s} {'gpu':>6s} {'client':>6s} {'core':>6s} {'wait':>6s} {'Gbound':>6s}")

def row(label, s):
    if not s:
        return f"{label:24s} (no frames)"
    g = f"{100 * s['gpu_bound']:5.0f}%" if s["gpu_bound"] >= 0 else f"{'-':>6s}"
    return (f"{label:24s} {s['frames']:7d} {s['fps_avg']:6.0f} {s['fps_1pct_low']:6.0f} {s['ms_p50']:6.2f} {s['ms_p95']:6.2f}"
            f" {s['ms_p99']:6.2f} {s['ms_max']:7.1f} {s['stutters']:4d} {s['gpu_med']:6.2f} {s['client_med']:6.2f}"
            f" {s['core_med']:6.2f} {s['wait_med']:6.2f} {g}")

def sections(rows, key, n=15):
    """[(median, p99, name)] of the GPU passes (key gpum) or CPU sections (cpum), largest first."""
    m = {}
    for r in rows:
        for k, v in r[key].items():
            m.setdefault(k, []).append(v)
    # (a name absent in some frames counts 0 there)
    out = [(pct(v + [0.0] * (len(rows) - len(v)), .5), pct(v + [0.0] * (len(rows) - len(v)), .99), k) for k, v in m.items()]
    return sorted(out, reverse=True)[:n]

def show_sections(title, sec):
    return f"  {title:22s} median     p99 [ms]\n" + "\n".join(f"  {k:22s} {a:7.3f} {b:7.3f}" for a, b, k in sec)

def summary(path, phases):
    head, rows = load(path)
    s = dict(file=os.path.basename(path), header=head, all=stats(rows),
             gpu=[dict(name=k, med=a, p99=b) for a, b, k in sections(rows, "gpum", 25)],
             cpu=[dict(name=k, med=a, p99=b) for a, b, k in sections(rows, "cpum", 25)], phases={})
    for ph in phases:
        t0, t1 = map(float, ph.split(':'))
        s["phases"][ph] = stats([r for r in rows if t0 <= r["t"] < t1])
    return s

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('-p', '--phase', action='append', default=[], help='sim-time range t0:t1')
    ap.add_argument('-m', '--marks', action='store_true', help='GPU passes')
    ap.add_argument('-c', '--cpu', action='store_true', help='CPU sections')
    ap.add_argument('-w', '--worst', action='store_true', help='slowest frames')
    ap.add_argument('-j', '--json', help='write a summary to this file')
    a = ap.parse_args()
    print(HDR)
    for path in a.files:
        head, rows = load(path)
        name = os.path.basename(path).rsplit('.', 1)[0]
        print(row(name, stats(rows)))
        for ph in a.phase:
            t0, t1 = map(float, ph.split(':'))
            print(row(f"  {ph}", stats([r for r in rows if t0 <= r["t"] < t1])))
        if a.marks:
            print(show_sections("GPU pass", sections(rows, "gpum")))
        if a.cpu:
            print(show_sections("CPU section", sections(rows, "cpum")))
        if a.worst:
            for r in sorted(rows, key=lambda r: -r["wall"])[:8]:
                top = sorted(r["cpum"].items(), key=lambda kv: -kv[1])[:4]
                print(f"  {r['wall']:7.1f} ms at t={r['t']:.1f} s (GPU {r['gpu']:.1f}, wait {r['wait']:.1f}"
                      + (f", {r['bound']}" if r["bound"] else "") + ")"
                      + ("  " + ", ".join(f"{k} {v:.1f}" for k, v in top) if top else ""))
    if a.json:
        out = dict(runs=[summary(p, a.phase) for p in a.files])
        run = os.path.join(os.path.dirname(os.path.abspath(a.files[0])), "run.json")
        if os.path.exists(run):
            out["run"] = json.load(open(run))
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
        print(f"summary: {a.json}")
