#!/usr/bin/env python3
"""The projected silhouette area of the fireball (fire and soot) against time, from a run.py --video
run: pixels in the upper part of the frame (above the sea's foam/wreck band: the top 62 % of the
frame) that differ from the frame before the break-up by more than a threshold (the ball over sky
and the far sea), as a fraction of the frame; the flames alone (bright, warm pixels) too.
  silhouette.py OUT.png LABEL=RUNDIR [LABEL=RUNDIR ...]   (also prints a table)"""
import json, os, sys
import numpy as np
from PIL import Image
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

def series(d):
    rj = json.load(open(os.path.join(d, "run.json"))); npre = int(round(rj["video"][0] * 30))
    vid = os.path.join(d, "video"); fs = sorted(f for f in os.listdir(vid) if f.startswith("f_"))
    ref = np.asarray(Image.open(os.path.join(vid, fs[max(npre - 3, 0)])).convert("RGB").resize((640, 360)), dtype=float)
    H = int(0.62 * 360)
    t, area, fire = [], [], []
    for i, f in enumerate(fs):
        im = np.asarray(Image.open(os.path.join(vid, f)).convert("RGB").resize((640, 360)), dtype=float)
        dif = np.abs(im - ref).max(axis=2)[:H]
        m = dif > 40
        r, g, b = im[:H, :, 0], im[:H, :, 1], im[:H, :, 2]
        fl = m & (r > 150) & (r > b + 40)
        t.append((i - npre) / 30.0); area.append(m.mean() * 0.62 * 100.0); fire.append(fl.mean() * 0.62 * 100.0)
    return np.array(t), np.array(area), np.array(fire)

def main():
    out = sys.argv[1]; runs = [a.split("=", 1) for a in sys.argv[2:]]
    fig, ax = plt.subplots(figsize=(9, 4))
    for lab, d in runs:
        t, a, f = series(d)
        ax.plot(t, a, "-", label="%s: ball (fire + soot)" % lab)
        ax.plot(t, f, "--", label="%s: flames" % lab, alpha=0.7)
        sel = (t > 0.1) & (t < 2.6)
        dd = np.diff(a[sel]); i = np.argmin(dd)
        print("%s: area max %.2f %% at %.2f s; largest frame-to-frame drop before 2.6 s: %.2f %% at %.2f s; area at 0.4 / 0.8 / 1.2 / 1.6 / 2.0 s: %s" % (
            lab, a.max(), t[np.argmax(a)], dd[i], t[sel][i + 1], " / ".join("%.2f" % a[np.argmin(np.abs(t - x))] for x in (0.4, 0.8, 1.2, 1.6, 2.0))))
    ax.axvline(0, color="k", ls=":"); ax.set_xlabel("t after the break-up [s]"); ax.set_ylabel("share of the frame [%]")
    ax.set_title("projected silhouette of the fireball (the top 62 % of the frame)"); ax.grid(alpha=0.3); ax.legend(fontsize=8)
    fig.tight_layout(); fig.savefig(out, dpi=110); print(out)

if __name__ == "__main__": main()
