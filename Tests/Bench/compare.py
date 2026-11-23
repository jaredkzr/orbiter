#!/usr/bin/env python3
"""Compare two benchmark runs (Tests/Bench/run.sh output directories, or their summary.json).

  compare.py base new [-t pct] [-s n]

Per flight found in both: the frame-rate and frame-time figures of each (the mean
over repeats, run.sh -n), the change in percent, and a verdict: "better" or "WORSE"
where the change is over the noise floor, the larger of -t (default 3 %) and twice
the spread between repeats of either side (max - min over mean; one run: -t
alone), and for times over 0.05 ms; then the -s (default 8) GPU passes and CPU sections that changed most [ms].
A warning where the two runs' devices, sizes or settings differ.
"""
import argparse, json, os, re, statistics

# (figure, label, larger is better)
FIGS = [("fps_avg", "avg fps", True), ("fps_1pct_low", "1% low fps", True), ("ms_p50", "p50 ms", False),
        ("ms_p95", "p95 ms", False), ("ms_p99", "p99 ms", False), ("stutters", "stutters", False),
        ("gpu_med", "GPU med ms", False), ("client_med", "client ms", False), ("core_med", "core ms", False)]
SAME = ["gpu", "driver", "size", "render", "aa", "rt", "rt_lighting", "rt_reflections", "clouds", "plumes",
        "particles", "particle_density", "smoke", "fireballs", "args", "env"]

def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "summary.json")
    d = json.load(open(path))
    flights = {}
    for r in d["runs"]:
        name = r["header"].get("flight") or re.sub(r"-r\d+$", "", r["file"].rsplit(".", 1)[0])
        flights.setdefault(name, []).append(r)
    return d.get("run", {}), flights

def mean_spread(runs, key):
    xs = [r["all"][key] for r in runs if r["all"] and r["all"].get(key, -1) >= 0]
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
    a = ap.parse_args()
    rb, fb = load(a.base)
    rn, fn = load(a.new)
    print(f"base: {rb.get('git', '?')}{'+' if rb.get('dirty') else ''} {rb.get('date', '')}")
    print(f"new:  {rn.get('git', '?')}{'+' if rn.get('dirty') else ''} {rn.get('date', '')}")
    worse = 0
    for name in sorted(set(fb) & set(fn)):
        B, N = fb[name], fn[name]
        hb, hn = B[0]["header"], N[0]["header"]
        diff = [k for k in SAME if hb.get(k) != hn.get(k)]
        print(f"\n{name} ({len(B)} vs {len(N)} runs)" + (f"  WARNING: differs in {', '.join(f'{k} ({hb.get(k)} -> {hn.get(k)})' for k in diff)}" if diff else ""))
        for key, label, up in FIGS:
            mb, sb = mean_spread(B, key)
            mn, sn = mean_spread(N, key)
            if mb is None or mn is None:
                continue
            if key == "stutters":
                # (a count: the change as such; frames over twice the median and 50 ms)
                verdict = "" if mn == mb else "better" if mn < mb else "WORSE"
                print(f"  {label:12s} {mb:9.1f} -> {mn:9.1f}  {mn - mb:+6.1f}            {verdict}")
                continue
            ch = 100 * (mn - mb) / mb if mb else 0.0
            floor = max(a.threshold, 200 * max(sb, sn))
            verdict = ""
            # (and at least 0.05 ms for a time: a tenth of a small part is no change)
            if abs(ch) > floor and not (key.endswith("_med") or key.startswith("ms_")) or abs(ch) > floor and abs(mn - mb) > 0.05:
                good = (ch > 0) == up
                verdict = "better" if good else "WORSE"
                worse += not good
            print(f"  {label:12s} {mb:9.2f} -> {mn:9.2f}  {ch:+6.1f}%  (floor {floor:.1f}%)  {verdict}")
        for key, title in (("gpu", "GPU passes"), ("cpu", "CPU sections")):
            sb, sn = sections(B, key), sections(N, key)
            ch = sorted(((sn.get(k, 0.0) - sb.get(k, 0.0), k) for k in set(sb) | set(sn)), key=lambda x: -abs(x[0]))
            if ch:
                print(f"  {title} changed most [ms]: " + ", ".join(f"{k} {sb.get(k, 0):.2f}->{sn.get(k, 0):.2f}" for d, k in ch[:a.sections]))
    for name in sorted(set(fb) ^ set(fn)):
        print(f"\n{name}: only in {'base' if name in fb else 'new'}")
    exit(1 if worse else 0)
