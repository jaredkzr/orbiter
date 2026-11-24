#!/usr/bin/env python3
"""Frame-rate and pacing statistics of ORBITER_VK_FRAMETIMES logs (Tests/Bench/run.sh).

  analyze.py file.ft ...                  table: frame rate, frame times and hitches per file
  analyze.py -p t0:t1 [-p ...] file.ft    the same per sim-time range
  analyze.py -m file.ft                   GPU passes (median/p99 ms), largest first
  analyze.py -c file.ft                   CPU sections (median/p99 ms), largest first
  analyze.py -n file.ft                   workload counters, heap use, GPU clocks and power
  analyze.py -w file.ft                   the slowest frames, with their largest CPU sections
  analyze.py -e REGEX file.ft             per script event matching REGEX ("bench: <event> at <sim t>"
                                          in the log): the hitch excess and recovery after it
  analyze.py -s SIM_S file.ft             measure from SIM_S after the first frame (not the marker)
  analyze.py -j out/summary.json f.ft ... a summary per file (and its phases) as JSON,
                                          with the run's stamp (compare.py reads it)

Lines (VulkanClient.cpp clbkDisplayFrame): a header "# orbiter-frametimes N key=value
..." (device, size, settings, the benchmark's stamp), then per frame: the frame, sim
time [s], wall time since the previous frame [ms], GPU total [ms], the GPU passes
(name=ms), "CPU render=" (the client, from clbkRenderScene to the frame's end), "CPU
wait=" (its waits on the GPU: frames in flight, the swapchain, and from version 3 idle
waits: vkDeviceWaitIdle, ImmediateSubmit, captures), then (version 2) "@cpu" and the
CPU's sections (name=ms: the core's state update "core pre/step/post", "core other",
"core input", clbkUpdate's, then the CPU time up to each GPU pass's mark - from version
3 "prepare" is the planets' tiles, scatter, cities and clouds (its parts "prep
terrain/scatter/cities/clouds" are within it, not added) and "shadows" the shadow maps
alone; "overlay", "2d ui", "submit") and "@bound=" G or C (the frame's wait over
max(0.5 ms, 5 %) of its wall time), then (version 3) "@glag=" the frames between this
line and the frame its GPU times are from, "@n" workload counters (tiles drawn/new/
freed, meshes made, idle waits, texture uploads, ray tracing builds, ...), "@mem used=
budget=" [MB] the device-local heap (the driver's: VK_EXT_memory_budget), "@hw" the
GPU's clocks [MHz], power [W], junction temperature and busy share (amdgpu).

The measured window: from the script's "bench: measure at <sim t>" in the run's log
(<name>.log beside the .ft) to the end; without one, all but the first SKIP frames
(older runs). The frames before it are the start-up: "load" is the log time of the
first frame [s], "stable" the wall time from the first frame until the first 2 s
without a frame over 33.3 ms.

Table: avg = frames / wall time; p50/p99/p99.9/max: frame times [ms]; >33/>50: the
share of wall time in frames over 33.3 / 50 ms [%]; exc: hitch excess, the time
frames took beyond max(2x median, 16.7 ms) [s per minute]; strk: the longest run of
frames over max(2x median, 33.3 ms); 1% low: the mean frame rate of the slowest 1% of
frames (secondary); cpu%/gpu%: median busy shares, (wall - wait)/wall and the frame's
own GPU total/wall (the bottleneck near 100); G%: frames bound by the GPU, held (a
wait over max(1 ms, 15 %) in 3 frames in a row); gpu, client (less the wait), core,
wait: medians [ms].
"""
import argparse, json, math, os, re, shlex, statistics

SKIP = 60  # (frames: only for logs without a measure marker)
MARK = re.compile(r"([A-Za-z0-9][A-Za-z0-9 ]*?)=(-?[\d.]+)")
EVENT = re.compile(r"^(\d+\.\d+): bench: (.*?) at ([\d.]+)\s*$")
VSYNC = 1000.0 / 60.0

def parse_marks(text):
    # (a pass marked more than once a frame, e.g. terrain per planet: summed)
    f = {}
    for k, v in MARK.findall(text):
        f[k.strip()] = f.get(k.strip(), 0.0) + float(v)
    return f

def section(rest, tag):
    """The text after "@tag" up to the next " @"."""
    m = re.search(r"@" + tag + r"\b", rest)
    if not m:
        return None
    s = rest[m.end():]
    j = s.find(" @")
    return s if j < 0 else s[:j]

def read_log(path):
    """(events [(name, sim t, log t)], first-frame log time or None) from the run's Orbiter.log copy."""
    ev, first = [], None
    if not os.path.exists(path):
        return ev, first
    for ln in open(path, errors="replace"):
        if first is None and "VulkanClient: first frame" in ln:
            try:
                first = float(ln.split(":", 1)[0])
            except ValueError:
                pass
        m = EVENT.match(ln.strip())
        if m:
            ev.append((m.group(2), float(m.group(3)), float(m.group(1))))
    return ev, first

def load(path, skip=None):
    """(header dict, rows, info): a row is a dict t, wall, gpu, gpu_own, render, wait,
    gpum, cpum, n, mem, hw, bound; rows of the measured window; info: the script's
    events, all rows, the start-up rows, the first frame's log time, the window's rule."""
    head, allrows = {}, []
    for ln in open(path, errors="replace"):
        if ln.startswith("#"):
            try:
                toks = shlex.split(ln[1:])
                if len(toks) > 1 and toks[0] == "orbiter-frametimes":
                    head["version"] = toks[1]
                for tok in toks[2:]:
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        head[k] = v
            except ValueError:
                pass
            continue
        p = ln.split()
        if len(p) < 4:
            continue
        body = " ".join(p[4:])
        gpart, _, rest = body.partition(" CPU render=")
        if not rest and body.startswith("CPU render="):
            gpart, rest = "", body[len("CPU render="):]
        m = re.match(r"([\d.]+) CPU wait=([\d.]+)", rest)
        cpart = (section(rest, "cpu") or "").split("@bound", 1)[0]
        b = re.search(r"@bound=([GC])", rest)
        g = re.search(r"@glag=(\d+)", rest)
        n, mem, hw = section(rest, "n"), section(rest, "mem"), section(rest, "hw")
        allrows.append(dict(t=float(p[1]), wall=float(p[2]), gpu=float(p[3]), gpu_own=-1.0,
                            render=float(m.group(1)) if m else -1.0, wait=float(m.group(2)) if m else 0.0,
                            gpum=parse_marks(gpart), cpum=parse_marks(cpart), bound=b.group(1) if b else None,
                            glag=int(g.group(1)) if g else None,
                            n=parse_marks(n) if n is not None else None,
                            mem=parse_marks(mem) if mem else None, hw=parse_marks(hw) if hw else None))
    # (a frame's GPU times are on a later line: frames in flight)
    for i, r in enumerate(allrows):
        if r["glag"] is not None and i - r["glag"] >= 0:
            allrows[i - r["glag"]]["gpu_own"] = r["gpu"]
    stem = path[:-3] if path.endswith(".ft") else path
    ev, first = read_log(stem + ".log")
    meas = [t for name, t, _ in ev if name == "measure"]
    if skip is not None:
        t0 = allrows[0]["t"] if allrows else 0.0
        k = next((i for i, r in enumerate(allrows) if r["t"] - t0 >= skip), len(allrows))
        how = f"sim +{skip:g} s"
    elif meas:
        k = next((i for i, r in enumerate(allrows) if r["t"] >= meas[0]), len(allrows))
        how = "marker"
    else:
        k, how = min(SKIP, len(allrows)), f"{SKIP} frames"
    info = dict(events=[e for e in ev if e[0] != "measure"], all=allrows, startup=allrows[:k], first=first,
                window=how, version=head.get("version", "1"))
    return head, allrows[k:], info

def pct(xs, q):
    s = sorted(xs)
    if not s:
        return 0.0
    k = (len(s) - 1) * q
    i = int(k)
    return s[i] if i + 1 >= len(s) else s[i] + (s[i + 1] - s[i]) * (k - i)

def held_bound(rows):
    """The share of frames bound by the GPU: the wait over max(1 ms, 15 %) of the frame,
    in at least 3 frames in a row (the logged flag lags and flips in balanced frames)."""
    run = g = 0
    for r in rows:
        ok = r["wait"] > max(1.0, 0.15 * r["wall"])
        run = run + 1 if ok else 0
        g += run >= 3
    return g / len(rows) if rows else -1

def hitches(w, med):
    tot = sum(w)
    lim = max(2 * med, 33.3)
    streak = best = 0
    sms = bms = 0.0
    for x in w:
        if x > lim:
            streak += 1
            sms += x
        else:
            streak, sms = 0, 0.0
        if streak > best:
            best, bms = streak, sms
    jit = [abs(w[i] - w[i - 1]) for i in range(1, len(w))]
    refr = [max(1, math.ceil(x / VSYNC - 1e-6)) for x in w]
    return dict(ms_p999=pct(w, .999), share33=100 * sum(x for x in w if x > 33.3) / tot,
                share50=100 * sum(x for x in w if x > 50) / tot,
                excess=sum(max(0.0, x - max(2 * med, VSYNC)) for x in w) / 1000 / (tot / 60000),
                streak=best, streak_ms=bms, jit_p99=pct(jit, .99) if jit else 0.0,
                vs60_repeat=100 * sum(n - 1 for n in refr) / sum(refr), vs60_fps=60 * len(w) / sum(refr))

# (summed over the window, per minute of wall time)
COUNTERS = ["idle", "idlems", "imm", "tex", "blas", "blastri", "tnew", "tfreed", "mplain", "mgpu", "mapplied",
            "deferred", "bufnew", "buffree", "memnew", "memnewmb"]

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
    tot_s = sum(dt) / 1000
    s = dict(frames=len(dt), wall_s=tot_s, fps_avg=1000 * len(dt) / sum(dt),
             ms_p50=med, ms_p95=pct(dt, 0.95), ms_p99=pct(dt, 0.99), ms_max=max(dt),
             fps_1pct_low=1000 * len(slow) / sum(slow),
             stutters=sum(1 for x in dt if x > 2 * med and x > 50),
             gpu_med=pct(gpu, 0.5), gpu_p99=pct(gpu, 0.99),
             # (the client's frame less its wait on the GPU; the core: its measured
             # parts where logged, else the wall time less the client's)
             client_med=pct([r["render"] - r["wait"] for r in cpu], 0.5) if cpu else -1,
             core_med=pct(core, 0.5) if core else (pct([r["wall"] - r["render"] for r in cpu], 0.5) if cpu else -1),
             wait_med=pct([r["wait"] for r in cpu], 0.5) if cpu else -1,
             gpu_bound_flag=sum(1 for b in bound if b == "G") / len(bound) if bound else -1,
             gpu_bound=held_bound(rows),
             cpu_busy=100 * pct([(r["wall"] - r["wait"]) / r["wall"] for r in rows if r["wall"] > 0], .5))
    own = [min(r["gpu_own"] / r["wall"], 1.5) for r in rows if r["wall"] > 0 and r["gpu_own"] > 0]
    if not own:  # (older logs: the GPU times as logged, a frame or two late)
        own = [min(r["gpu"] / r["wall"], 1.5) for r in rows if r["wall"] > 0 and r["gpu"] > 0]
    s["gpu_busy"] = 100 * pct(own, .5) if own else -1
    s.update(hitches(dt, med))
    ns = [r["n"] for r in rows if r["n"] is not None]
    if ns:
        for k in COUNTERS:
            s["n_" + k] = sum(n.get(k, 0.0) for n in ns) / (tot_s / 60)
        s["n_tdrawn"] = pct([n.get("tdrawn", 0.0) for n in ns], .5)
        s["n_tdraws"] = pct([n.get("tdraws", 0.0) for n in ns], .5)
        s["n_tmeshes_max"] = max(n.get("tmeshes", 0.0) for n in ns)
        s["n_tmb_max"] = max(n.get("tmb", 0.0) for n in ns)
        s["n_mpending_max"] = max(n.get("mpending", 0.0) for n in ns)
    ms = [r["mem"] for r in rows if r["mem"]]
    if ms:
        s["mem_used_max"] = max(m.get("used", 0) for m in ms)
        s["mem_used_med"] = pct([m.get("used", 0) for m in ms], .5)
        s["mem_budget"] = pct([m.get("budget", 0) for m in ms], .5)
        if any("rss" in m for m in ms):
            s["mem_rss_max"] = max(m.get("rss", 0) for m in ms)
        # (long runs: the growth after the first 5 minutes [MB per minute], a least-squares line)
        if tot_s > 600:
            acc, pts = 0.0, []
            for r in rows:
                acc += r["wall"] / 1000
                if acc >= 300 and r["mem"]:
                    pts.append((acc / 60, r["mem"].get("used", 0), r["mem"].get("rss", -1)))
            def slope(xy):
                n = len(xy)
                if n < 10:
                    return 0.0
                mx, my = sum(x for x, _ in xy) / n, sum(y for _, y in xy) / n
                sxx = sum((x - mx) ** 2 for x, _ in xy)
                return sum((x - mx) * (y - my) for x, y in xy) / sxx if sxx else 0.0
            s["mem_slope"] = slope([(t, u) for t, u, _ in pts])
            if all(r >= 0 for _, _, r in pts):
                s["rss_slope"] = slope([(t, r) for t, _, r in pts])
    hw = [r["hw"] for r in rows if r["hw"]]
    for k in ("sclk", "mclk", "watts", "tj", "busy"):
        xs = [h[k] for h in hw if k in h]
        if xs:
            s["hw_" + k] = pct(xs, .5)
    return s

def startup(info):
    """The load time [s] (the first frame's log time) and the time to stable [s]: wall
    time from the first frame to the first 2 s without a frame over 33.3 ms."""
    w = [r["wall"] for r in info["all"]]
    acc, i0, stable = 0.0, 1, None
    for i, x in enumerate(w):
        if x > 33.3:
            i0, acc = i + 1, 0.0
        else:
            acc += x
            if acc >= 2000:
                stable = sum(w[1:i0]) / 1000
                break
    return dict(load_s=info["first"] if info["first"] is not None else -1,
                stable_s=stable if stable is not None else -1,
                startup_frames=len(info["startup"]), startup_s=sum(r["wall"] for r in info["startup"]) / 1000,
                startup_worst_ms=max([r["wall"] for r in info["startup"]] or [0.0]))

def events(rows, evs, regex=None, window=6.0):
    """Per event: the hitch excess until the next event (at most window s of sim time)
    [s], the recovery [s wall]: until the first 0.5 s of frames without one over
    max(2x median, 33.3 ms), and the worst frame."""
    if not rows:
        return []
    med = pct([r["wall"] for r in rows], .5)
    lim = max(2 * med, 33.3)
    out = []
    for name, t, _ in evs:
        if regex and not re.search(regex, name):
            continue
        tnext = min([e[1] for e in evs if e[1] > t] + [t + window])
        sub = [r for r in rows if t <= r["t"] < tnext]
        if not sub:
            continue
        exc = sum(max(0.0, r["wall"] - max(2 * med, VSYNC)) for r in sub) / 1000
        acc, rec, wall = 0.0, None, 0.0
        for r in sub:
            if r["wall"] > lim:
                acc, rec = 0.0, None
            elif rec is None:
                rec, acc = wall, r["wall"]
            else:
                acc += r["wall"]
            wall += r["wall"]
            if rec is not None and acc >= 500:
                break
        out.append(dict(event=name, t=t, excess_s=exc, recovery_s=(rec if rec is not None else wall) / 1000,
                        worst_ms=max(r["wall"] for r in sub)))
    return out

HDR = (f"{'':22s} {'frames':>6s} {'avg':>5s} {'p50':>6s} {'p99':>6s} {'p99.9':>6s} {'max':>6s} {'>33%':>5s} {'>50%':>5s}"
       f" {'exc':>5s} {'strk':>4s} {'1%low':>5s} {'cpu%':>4s} {'gpu%':>4s} {'G%':>4s} {'gpu':>6s} {'client':>6s} {'core':>6s} {'wait':>6s}")

def row(label, s):
    if not s:
        return f"{label:22s} (no frames)"
    g = f"{100 * s['gpu_bound']:4.0f}" if s["gpu_bound"] >= 0 else f"{'-':>4s}"
    return (f"{label[-22:]:22s} {s['frames']:6d} {s['fps_avg']:5.0f} {s['ms_p50']:6.2f} {s['ms_p99']:6.2f} {s['ms_p999']:6.1f}"
            f" {s['ms_max']:6.1f} {s['share33']:5.1f} {s['share50']:5.1f} {s['excess']:5.2f} {s['streak']:4d} {s['fps_1pct_low']:5.0f}"
            f" {s['cpu_busy']:4.0f} {s['gpu_busy']:4.0f} {g} {s['gpu_med']:6.2f} {s['client_med']:6.2f} {s['core_med']:6.2f} {s['wait_med']:6.2f}")

def sections(rows, key, n=15):
    """[(median, p99, name)] of the GPU passes (key gpum) or CPU sections (cpum), largest first."""
    m = {}
    for r in rows:
        for k, v in (r[key] or {}).items():
            m.setdefault(k, []).append(v)
    # (a name absent in some frames counts 0 there)
    out = [(pct(v + [0.0] * (len(rows) - len(v)), .5), pct(v + [0.0] * (len(rows) - len(v)), .99), k) for k, v in m.items()]
    return sorted(out, reverse=True)[:n]

def show_sections(title, sec):
    return f"  {title:22s} median     p99 [ms]\n" + "\n".join(f"  {k:22s} {a:7.3f} {b:7.3f}" for a, b, k in sec)

def show_counters(s):
    if not s:
        return ""
    keys = [k for k in s if k.startswith(("n_", "mem_", "hw_"))]
    return "  " + "\n  ".join(", ".join(f"{k}={s[k]:.4g}" for k in keys[i:i + 6]) for i in range(0, len(keys), 6))

def summary(path, phases, skip=None):
    head, rows, info = load(path, skip)
    s = dict(file=os.path.basename(path), header=head, window=info["window"], all=stats(rows),
             startup=startup(info), events=events(rows, info["events"]),
             gpu=[dict(name=k, med=a, p99=b) for a, b, k in sections(rows, "gpum", 25)],
             cpu=[dict(name=k, med=a, p99=b) for a, b, k in sections(rows, "cpum", 25)], phases={})
    if s["all"]:
        s["all"].update({k: v for k, v in s["startup"].items() if k in ("load_s", "stable_s")})
        if s["events"]:
            s["all"]["event_excess"] = statistics.mean(e["excess_s"] for e in s["events"])
            s["all"]["event_recovery"] = statistics.mean(e["recovery_s"] for e in s["events"])
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
    ap.add_argument('-n', '--counters', action='store_true', help='workload counters, memory, GPU clocks')
    ap.add_argument('-w', '--worst', action='store_true', help='slowest frames')
    ap.add_argument('-e', '--events', help='script events matching this regex: excess and recovery')
    ap.add_argument('-s', '--skip', type=float, help='measure from this many sim seconds after the first frame')
    ap.add_argument('-j', '--json', help='write a summary to this file')
    a = ap.parse_args()
    print(HDR)
    for path in a.files:
        head, rows, info = load(path, a.skip)
        name = os.path.basename(path).rsplit('.', 1)[0]
        s = stats(rows)
        print(row(name, s))
        su = startup(info)
        print(f"  (window: {info['window']}; start-up: first frame at {su['load_s']:.1f} s in the log, stable after"
              f" {su['stable_s']:.1f} s; {su['startup_frames']} frames in {su['startup_s']:.1f} s before the window, worst {su['startup_worst_ms']:.0f} ms)")
        for ph in a.phase:
            t0, t1 = map(float, ph.split(':'))
            print(row(f"  {ph}", stats([r for r in rows if t0 <= r["t"] < t1])))
        if a.marks:
            print(show_sections("GPU pass", sections(rows, "gpum")))
        if a.cpu:
            print(show_sections("CPU section", sections(rows, "cpum")))
        if a.counters:
            print(show_counters(s))
        if a.events is not None:
            for e in events(rows, info["events"], a.events):
                print(f"  event {e['event'][:40]:40s} at {e['t']:8.2f}: excess {e['excess_s']:6.3f} s, recovery {e['recovery_s']:5.2f} s, worst {e['worst_ms']:6.1f} ms")
        if a.worst:
            for r in sorted(rows, key=lambda r: -r["wall"])[:8]:
                top = sorted(r["cpum"].items(), key=lambda kv: -kv[1])[:4]
                g = r["gpu_own"] if r["gpu_own"] > 0 else r["gpu"]
                print(f"  {r['wall']:7.1f} ms at t={r['t']:.1f} s (GPU {g:.1f}, wait {r['wait']:.1f}"
                      + (f", {r['bound']}" if r["bound"] else "") + ")"
                      + ("  " + ", ".join(f"{k} {v:.1f}" for k, v in top) if top else ""))
    if a.json:
        out = dict(runs=[summary(p, a.phase, a.skip) for p in a.files])
        run = os.path.join(os.path.dirname(os.path.abspath(a.files[0])), "run.json")
        if os.path.exists(run):
            out["run"] = json.load(open(run))
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
        print(f"summary: {a.json}")
