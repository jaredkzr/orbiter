#!/usr/bin/env python3
"""Compare two benchmark runs (Tests/Bench/run.sh output directories, or their summary.json).

  compare.py base new [-t pct] [-s n] [-a]

Per flight found in both: the figures of each (the mean over repeats, run.sh -n), the
change, and a verdict, "better" or "WORSE", where the change is over the noise floor:
- rates and times: the larger of -t (default 3 %) and twice the spread between repeats
  of either side (max - min over mean; one run: -t alone), and for times at least 0.05 ms;
- hitch shares (>33/>50 ms, % of wall time) and hitch excess (s/min): twice the
  spread, and at least 0.3 points / 0.05 s per minute;
- counts (stutters, the longest slow streak): Poisson, 2 sqrt(base + new) (per repeat);
- start-up (load, time to stable [s]): twice the spread and at least 0.3 s.
Hitch figures from a single real-time run are noisy (a hitch makes the next step
longer): compare fixed-step runs (run.sh -F) or repeats (-n 3). Then the -s (default 8)
GPU passes and CPU sections that changed most [ms]; -a: also the workload counters,
memory and GPU clocks (no verdict). A warning where the two runs' devices, sizes,
settings or scenario/script hashes differ.
"""
import argparse, json, math, os, re, statistics

# (figure, label, larger is better, kind: rel = relative %, abs = absolute floor, count, start)
FIGS = [("fps_avg", "avg fps", True, "rel"), ("ms_p50", "p50 ms", False, "rel"), ("ms_p99", "p99 ms", False, "rel"),
        ("ms_p999", "p99.9 ms", False, "rel"), ("share33", ">33ms % wall", False, "abs"),
        ("share50", ">50ms % wall", False, "abs"), ("excess", "excess s/min", False, "abs"),
        ("streak", "slow streak", False, "count"), ("stutters", "stutters", False, "count"),
        ("fps_1pct_low", "1% low fps", True, "rel"),
        ("gpu_med", "GPU med ms", False, "rel"), ("client_med", "client ms", False, "rel"), ("core_med", "core ms", False, "rel"),
        ("cpu_busy", "CPU busy %", None, "rel"), ("gpu_busy", "GPU busy %", None, "rel"),
        ("event_excess", "event exc s", False, "abs"), ("event_recovery", "event rec s", False, "abs"),
        ("load_s", "load s", False, "start"), ("stable_s", "stable s", False, "start")]
ABS_FLOOR = {"share33": 0.3, "share50": 0.3, "excess": 0.05, "event_excess": 0.03, "event_recovery": 0.05}
SAME = ["gpu", "driver", "size", "render", "aa", "rt", "rt_lighting", "rt_reflections", "clouds", "plumes",
        "particles", "particle_density", "smoke", "fireballs", "args", "env", "scn_hash", "lua_hash"]

def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "summary.json")
    d = json.load(open(path))
    flights = {}
    for r in d["runs"]:
        name = r["header"].get("flight") or re.sub(r"-r\d+$", "", r["file"].rsplit(".", 1)[0])
        flights.setdefault(name, []).append(r)
    return d.get("run", {}), flights

def values(runs, key):
    return [r["all"][key] for r in runs if r["all"] and r["all"].get(key, -1) is not None and r["all"].get(key, -1) >= 0]

def mean_spread(xs):
    if not xs:
        return None, 0.0
    m = statistics.mean(xs)
    return m, ((max(xs) - min(xs)) / abs(m) if len(xs) > 1 and m else 0.0)

def sections(runs, key):
    out = {}
    for r in runs:
        for s in r[key]:
            out.setdefault(s["name"], []).append(s["med"])
    return {k: statistics.mean(v) for k, v in out.items()}

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base')
    ap.add_argument('new')
    ap.add_argument('-t', '--threshold', type=float, default=3.0, help='least change counted [%%]')
    ap.add_argument('-s', '--sections', type=int, default=8, help='passes and sections listed')
    ap.add_argument('-a', '--all', action='store_true', help='also the counters, memory and GPU clocks')
    a = ap.parse_args()
    rb, fb = load(a.base)
    rn, fn = load(a.new)
    print(f"base: {rb.get('git', '?')}{'+' if rb.get('dirty') else ''} {rb.get('date', '')} {rb.get('mode', '')}")
    print(f"new:  {rn.get('git', '?')}{'+' if rn.get('dirty') else ''} {rn.get('date', '')} {rn.get('mode', '')}")
    worse = 0
    for name in sorted(set(fb) & set(fn)):
        B, N = fb[name], fn[name]
        hb, hn = B[0]["header"], N[0]["header"]
        diff = [k for k in SAME if hb.get(k) != hn.get(k)]
        print(f"\n{name} ({len(B)} vs {len(N)} runs)" + (f"  WARNING: differs in {', '.join(f'{k} ({hb.get(k)} -> {hn.get(k)})' for k in diff)}" if diff else ""))
        for key, label, up, kind in FIGS:
            xb, xn = values(B, key), values(N, key)
            mb, sb = mean_spread(xb)
            mn, sn = mean_spread(xn)
            if mb is None or mn is None:
                continue
            d = mn - mb
            verdict = ""
            if kind == "count":
                # (Poisson: counts per run, the floor 2 sqrt(b + n) over the repeats)
                floor = 2 * math.sqrt((mb + mn) / max(1, min(len(xb), len(xn))))
                sig = abs(d) > max(floor, 0.5)
                line = f"  {label:13s} {mb:9.1f} -> {mn:9.1f}  {d:+8.1f}   (floor {floor:.1f})"
            elif kind in ("abs", "start"):
                spread = max(max(xb) - min(xb), max(xn) - min(xn)) if len(xb) > 1 or len(xn) > 1 else 0.0
                floor = max(2 * spread, ABS_FLOOR.get(key, 0.3))
                sig = abs(d) > floor
                line = f"  {label:13s} {mb:9.3f} -> {mn:9.3f}  {d:+8.3f}   (floor {floor:.3f})"
            else:
                ch = 100 * d / mb if mb else 0.0
                floor = max(a.threshold, 200 * max(sb, sn))
                # (and at least 0.05 ms for a time: a tenth of a small part is no change)
                sig = abs(ch) > floor and (not (key.endswith("_med") or key.startswith("ms_")) or abs(d) > 0.05)
                line = f"  {label:13s} {mb:9.2f} -> {mn:9.2f}  {ch:+7.1f}%   (floor {floor:.1f}%)"
            if sig and up is not None:
                good = (d > 0) == up
                verdict = "better" if good else "WORSE"
                worse += not good
            print(line + "  " + verdict)
        if a.all:
            keys = sorted({k for r in B + N if r["all"] for k in r["all"] if k.startswith(("n_", "mem_", "hw_"))})
            for k in keys:
                mb, _ = mean_spread(values(B, k))
                mn, _ = mean_spread(values(N, k))
                if mb is None or mn is None:
                    continue
                print(f"  {k:18s} {mb:11.4g} -> {mn:11.4g}")
        for key, title in (("gpu", "GPU passes"), ("cpu", "CPU sections")):
            sb, sn = sections(B, key), sections(N, key)
            ch = sorted(((sn.get(k, 0.0) - sb.get(k, 0.0), k) for k in set(sb) | set(sn)), key=lambda x: -abs(x[0]))
            if ch:
                print(f"  {title} changed most [ms]: " + ", ".join(f"{k} {sb.get(k, 0):.2f}->{sn.get(k, 0):.2f}" for d, k in ch[:a.sections]))
    for name in sorted(set(fb) ^ set(fn)):
        print(f"\n{name}: only in {'base' if name in fb else 'new'}")
    exit(1 if worse else 0)
