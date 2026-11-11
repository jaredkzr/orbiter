"""The live particles by class after the break-up (VK particles: ... by class:, ORBITER_VK_PARTICLE_STATS):
classes.py RUNDIR [t,t,...]  -> one line per time: the nearest log line at or after it. "Nothing lingers"
in vacuum is a number: no fire, fbsmoke, wreck, trail or steam particles alive some seconds after the burst."""
import sys, os, json, re
d = sys.argv[1]
ts = [float(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "1,2,5,10,20").split(",")]
tb = json.load(open(os.path.join(d, "run.json")))["breakup_simt"]
rows = []
for line in open(os.path.join(d, "orbiter.log"), errors="replace"):
    m = re.search(r"VK particles: simt=([\d.]+) by class:(.*)$", line)
    if m: rows.append((float(m.group(1)) - tb, m.group(2).strip()))
for t in ts:
    r = next((x for x in rows if x[0] >= t - 0.05), None)
    print(f"{os.path.basename(d.rstrip('/'))} +{t:g} s:", f"(+{r[0]:.1f}) {r[1]}" if r else "-")
