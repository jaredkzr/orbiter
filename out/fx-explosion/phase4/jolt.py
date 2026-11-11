#!/usr/bin/env python3
"""The camera jolt of a run (run.py --video): the logged jolt angles (ORBITER_VK_JOLT_LOG) and the
vertical shift of the horizon measured in the video frames (a strip at the frame's left edge, away
from the fireball, cross-correlated against the frame before the break-up), against time after
the break-up; a plot and a table.
  jolt.py RUNDIR [RUNDIR_B ...] OUT.png"""
import json, os, re, sys
import numpy as np
from PIL import Image
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

def profile(path, cols):
    im = np.asarray(Image.open(path).convert("L"), dtype=np.float64)
    return im[:, :cols].mean(axis=1)

def band_shift(ref, cur, lo, hi, maxs=30):
    # the vertical shift of the band [lo, hi) of the profile cur against ref (pixels; normalised
    # cross-correlation, sub-pixel by a parabola through the best three)
    r = ref[lo:hi]; r = (r - r.mean()) / (r.std() + 1e-6)
    best, scores = None, {}
    for sh in range(-maxs, maxs + 1):
        c = cur[lo + sh:hi + sh]; c = (c - c.mean()) / (c.std() + 1e-6)
        v = float((r * c).mean()); scores[sh] = v
        if best is None or v > scores[best]: best = sh
    if -maxs < best < maxs:
        y0, y1, y2 = scores[best - 1], scores[best], scores[best + 1]
        den = y0 - 2.0 * y1 + y2
        return best + ((y0 - y2) / (2.0 * den) if den != 0 else 0.0)
    return float(best)

def analyse(d):
    rj = json.load(open(os.path.join(d, "run.json")))
    tb, pre = rj["breakup_simt"], rj["video"][0]
    vid = os.path.join(d, "video")
    fs = sorted(f for f in os.listdir(vid) if f.startswith("f_"))
    npre = int(round(pre * 30))
    # (the strip: the leftmost 4 % of the frame - farther in, the fireball's heat shimmer
    # wobbles the horizon too; the runs for this plot have the shimmer off). The shift of
    # each frame against the one before (the waves move under a pixel a frame; the camera's
    # jolt is a step of several): the camera's motion frame by frame
    cols = max(int(0.04 * Image.open(os.path.join(vid, fs[0])).width), 30)
    prof = [profile(os.path.join(vid, f), cols) for f in fs]
    n = len(prof[0]); sm = np.convolve(prof[0], np.ones(5) / 5.0, mode="same")
    h0 = int(0.3 * n) + int(np.argmax(sm[int(0.3 * n):int(0.7 * n)]))
    lo, hi = h0 - 40, min(h0 + 200, n - 40)
    t = []; px = []
    for i, f in enumerate(fs):
        t.append((i - npre) / 30.0)  # (frame index relative to the break-up: the ring holds pre s before it)
        px.append(band_shift(prof[i - 1], prof[i], lo, hi) if i > 0 else 0.0)
    # the logged angles
    lt, lp, lr, arr, amp, dist = [], [], [], None, None, None
    for line in open(os.path.join(d, "orbiter.log")):
        m = re.search(r"jolt at ([0-9.]+): pitch ([-0-9.]+) deg roll ([-0-9.]+) deg", line)
        if m: lt.append(float(m.group(1)) - tb); lp.append(float(m.group(2))); lr.append(float(m.group(3)))
        m = re.search(r"jolt arrived at ([0-9.]+) from ([0-9]+) m: ([0-9.]+) deg", line)
        if m: arr, dist, amp = float(m.group(1)) - tb, float(m.group(2)), float(m.group(3))
    return dict(name=os.path.basename(d), t=t, px=px, lt=lt, lp=lp, lr=lr, arrive=arr, dist=dist, amp=amp)

def main():
    dirs, out = sys.argv[1:-1], sys.argv[-1]
    res = [analyse(d) for d in dirs]
    fig, axes = plt.subplots(len(res), 1, figsize=(9, 3.2 * len(res)), squeeze=False)
    for ax, r in zip(axes[:, 0], res):
        ax.plot(r["t"], r["px"], "-o", ms=3, color="#1f77b4", label="frame-to-frame vertical shift of the horizon strip [px]")
        if r["lt"]:
            ax2 = ax.twinx()
            ax2.plot(r["lt"], r["lp"], "-s", ms=3, color="#d62728", label="logged pitch [deg]")
            ax2.set_ylabel("deg"); ax2.legend(loc="upper right")
        ax.axvline(0.0, color="k", ls=":", label="flash (break-up)")
        if r["arrive"] is not None:
            ax.axvline(r["arrive"], color="#2ca02c", ls="--", label="sound arrives: %.2f s (%.0f m / 340)" % (r["arrive"], r["dist"]))
        ax.set_title("%s: %s" % (r["name"], ("jolt %.2f deg at %.0f m" % (r["amp"], r["dist"])) if r["amp"] is not None else "no jolt (none logged)"))
        ax.set_xlabel("t after the break-up [s]"); ax.set_ylabel("px"); ax.legend(loc="upper left"); ax.grid(alpha=0.3)
    fig.tight_layout(); fig.savefig(out, dpi=110)
    print(out)
    for r in res:
        print(r["name"], "arrive", r["arrive"], "amp", r["amp"], "dist", r["dist"])
        print("  t[s]  px/frame | logged pitch")
        for t, px in zip(r["t"], r["px"]):
            if (r["arrive"] is not None and r["arrive"] - 0.15 < t < r["arrive"] + 0.45) or abs(px) > 1.5:
                lp = [p for lt, p in zip(r["lt"], r["lp"]) if abs(lt - t) < 0.017]
                print("  %5.2f %6.1f | %s" % (t, px, ("%.3f" % lp[0]) if lp else "-"))

if __name__ == "__main__": main()
