"""Per-frame screen motion of the explosion (fixed observer): the mask of what is not
sky above the horizon; its area, top, centroid height and width, and their rates.
motion.py RUNDIR [RUNDIR2 ...] OUT.png   (RUNDIR/video/f_*.png, RUNDIR/run.json)"""
import sys, os, json
import numpy as np
from PIL import Image
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

def analyse(d):
    meta = json.load(open(os.path.join(d, "run.json")))
    pre = meta["video"][0] if meta.get("video") else 0.0
    fs = sorted(f for f in os.listdir(d + "/video") if f.startswith("f_"))
    im0 = np.asarray(Image.open(os.path.join(d, "video", fs[0])).convert("RGB")).astype(float)
    H, W, _ = im0.shape
    # the horizon: the first row from the top whose mean is not sky-blue
    blue = im0[:, :, 2] - im0[:, :, 0]
    rows = blue.mean(axis=1)
    hz = int(np.argmax(rows < 20)) - 4
    ts, area, top, cy, width = [], [], [], [], []
    for i, f in enumerate(fs):
        im = np.asarray(Image.open(os.path.join(d, "video", f)).convert("RGB")).astype(float)[:hz]
        diff = np.abs(im - im0[:hz]).sum(axis=2)
        m = diff > 40
        t = i / 30.0 - pre
        ys, xs = np.nonzero(m)
        ts.append(t)
        if len(ys) < 50:
            area.append(0); top.append(np.nan); cy.append(np.nan); width.append(0); continue
        area.append(len(ys) / (W * H) * 100)
        top.append((hz - np.percentile(ys, 1)) / H * 100)
        cy.append((hz - ys.mean()) / H * 100)
        width.append((np.percentile(xs, 99) - np.percentile(xs, 1)) / W * 100)
    return np.array(ts), np.array(area), np.array(top), np.array(cy), np.array(width)

out = sys.argv[-1]
fig, ax = plt.subplots(2, 2, figsize=(13, 8))
for d in sys.argv[1:-1]:
    t, a, tp, c, w = analyse(d)
    lab = os.path.basename(d.rstrip("/"))
    ax[0, 0].plot(t, a, label=lab); ax[0, 0].set_title("area [% of frame]")
    ax[0, 1].plot(t, tp, label=lab); ax[0, 1].set_title("top above horizon [% of height]")
    ax[1, 0].plot(t[1:], np.diff(tp) * 30, label=lab + " top"); ax[1, 0].plot(t[1:], np.diff(c) * 30, "--", label=lab + " centroid")
    ax[1, 0].set_title("rise rate [% height / s]"); ax[1, 0].set_ylim(-20, 60)
    ax[1, 1].plot(t[1:], np.diff(w) * 30, label=lab); ax[1, 1].set_title("width growth [% width / s]"); ax[1, 1].set_ylim(-20, 120)
    with open(out.replace(".png", "_%s.txt" % lab), "w") as fo:
        for row in zip(t, a, tp, c, w): fo.write("%.3f %.2f %.2f %.2f %.2f\n" % row)
for a_ in ax.flat: a_.grid(alpha=0.3); a_.legend(fontsize=7); a_.set_xlabel("t after break-up [s]")
plt.tight_layout(); plt.savefig(out, dpi=90)
