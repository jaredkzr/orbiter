#!/usr/bin/env python3
"""Particle budget of the launch flights (Tests/Bench/run.sh Launch Launch8 LaunchFar).

  budget.py [--scale k] out_dir

For each flight found in out_dir (<name>.ft, <name>.log):
- GPU time of all particle passes per frame (the "pt *" marks and the
  screen-space smoke's "particles"), median and p95 over the flight's range,
  against the budget (DECISIONS B7, reference: RX 9070 XT, 2560x1440, FSR 3
  at 1.0, ray tracing on; --scale multiplies the budgets for other GPUs):
    Launch     T 0-60 s, camera 850 m from the pad:  <= 0.70 ms median, <= 0.90 ms p95
    Launch8    the same at density 8:                 time reported only
    LaunchFar  T 0-240 s, from 42 km (nothing within 22 km): <= 0.25 ms median
- the pool's health from the ORBITER_VK_PARTICLE_STATS lines: allocation
  failures and dropped emissions summed: must be 0 (all three flights).
Exit status 1 if any check fails.
"""
import argparse, os, re, sys

SKIP = 60
PT = ["pt update", "pt emit", "pt shadow", "pt light", "pt volume", "pt sort", "pt march half", "particles", "pt march", "pt sprites"]
FLIGHTS = {  # name: (t0, t1, median budget, p95 budget)
	"Launch": (0.0, 60.0, 0.70, 0.90),
	"Launch8": (0.0, 60.0, None, None),
	"LaunchFar": (0.0, 240.0, 0.25, None),
}

def pct(xs, q):
	s = sorted(xs)
	if not s: return float("nan")
	k = (len(s) - 1) * q; i = int(k)
	return s[i] if i + 1 >= len(s) else s[i] + (s[i + 1] - s[i]) * (k - i)

def particle_ms(path, t0, t1):
	out = []
	for i, ln in enumerate(open(path)):
		p = ln.split()
		if i < SKIP or len(p) < 4: continue
		t = float(p[1])
		if not (t0 <= t < t1): continue
		marks = dict()
		for k, v in re.findall(r"([A-Za-z][A-Za-z ]*?)=([\d.]+)", " ".join(ln.split(" CPU render")[0].split()[4:])):
			marks[k.strip()] = marks.get(k.strip(), 0.0) + float(v)
		out.append(sum(marks.get(k, 0.0) for k in PT))
	return out

def pool_health(path):
	fails = dropped = lines = 0
	for ln in open(path, errors="replace"):
		if "VK particles: simt=" not in ln: continue
		lines += 1
		m = re.search(r"allocFails=(\d+)", ln); fails += int(m.group(1)) if m else 0
		m = re.search(r"dropped (\d+)", ln); dropped += int(m.group(1)) if m else 0
	return lines, fails, dropped

def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--scale", type=float, default=1.0, help="budget factor for other GPUs")
	ap.add_argument("dir")
	a = ap.parse_args()
	ok, found = True, 0
	for name, (t0, t1, bmed, b95) in FLIGHTS.items():
		ft, log = os.path.join(a.dir, name + ".ft"), os.path.join(a.dir, name + ".log")
		if not os.path.exists(ft): continue
		found += 1
		ms = particle_ms(ft, t0, t1)
		med, p95 = pct(ms, 0.5), pct(ms, 0.95)
		res = []
		if bmed is not None:
			good = med <= bmed * a.scale
			res.append(f"median {med:.3f} ms (<= {bmed * a.scale:.2f}) {'ok' if good else 'OVER'}"); ok &= good
		else: res.append(f"median {med:.3f} ms")
		if b95 is not None:
			good = p95 <= b95 * a.scale
			res.append(f"p95 {p95:.3f} ms (<= {b95 * a.scale:.2f}) {'ok' if good else 'OVER'}"); ok &= good
		else: res.append(f"p95 {p95:.3f} ms")
		lines, fails, dropped = pool_health(log) if os.path.exists(log) else (0, 0, 0)
		if lines == 0:
			res.append("no particle telemetry (ORBITER_VK_PARTICLE_STATS) FAIL"); ok = False
		else:
			good = fails == 0 and dropped == 0
			res.append(f"allocFails {fails}, dropped {dropped} {'ok' if good else 'FAIL'}"); ok &= good
		print(f"{name:10s} T {t0:.0f}-{t1:.0f} s, {len(ms)} frames: " + "; ".join(res))
	if not found:
		print("no launch flights in", a.dir); return 1
	print("particle budget:", "met" if ok else "NOT met")
	return 0 if ok else 1

if __name__ == "__main__":
	sys.exit(main())
