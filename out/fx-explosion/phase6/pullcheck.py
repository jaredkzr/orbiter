#!/usr/bin/env python3
"""LOD pull-away check (run.py --pull ... --video): per video frame the fireball's bright
(fire) pixel count and its dark (soot) pixel count over the sky above the horizon, and the
frame-to-frame change of each: a pop at a level switch shows as a spike. Prints the frames
whose change is over 3x the median change, and writes a plot. pullcheck.py RUNDIR OUT.png"""
import json, os, sys
import numpy as np
from PIL import Image
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
d, out = sys.argv[1], sys.argv[2]
rj = json.load(open(os.path.join(d, "run.json")))
pre = rj["video"][0]
vid = os.path.join(d, "video")
fs = sorted(f for f in os.listdir(vid) if f.startswith("f_"))
fire, soot, t = [], [], []
for i, f in enumerate(fs):
    im = np.asarray(Image.open(os.path.join(vid, f)).convert("RGB"), dtype=np.float64)
    H = im.shape[0]
    sky = im[: int(H * 0.62)]  # (above the horizon in these shots)
    r, g, b = sky[..., 0], sky[..., 1], sky[..., 2]
    fire.append(int(((r > 150) & (r > g * 1.25) & (g > b * 1.3)).sum()))
    lum = sky.mean(axis=2)
    soot.append(int(((lum < 90) & (np.abs(r - b) < 25)).sum()))
    t.append(i / 30.0 - pre)
fire, soot, t = np.array(fire), np.array(soot), np.array(t)
tot = fire + soot
ch = np.abs(np.diff(tot)) / np.maximum(tot[:-1], 50)
med = np.median(ch[ch > 0]) if (ch > 0).any() else 0
print(f"frames {len(fs)}; median relative change per frame {med:.3f}")
for i in np.argsort(-ch)[:8]:
    print(f"  t {t[i+1]:+.2f} s: fire {fire[i]} -> {fire[i+1]}, soot {soot[i]} -> {soot[i+1]} (change {ch[i]:.2f})")
fig, ax = plt.subplots(2, 1, figsize=(9, 6), sharex=True)
ax[0].plot(t, fire, label="fire pixels"); ax[0].plot(t, soot, label="soot pixels"); ax[0].set_yscale("symlog"); ax[0].legend()
ax[1].plot(t[1:], ch); ax[1].set_ylabel("relative change / frame"); ax[1].set_xlabel("t after the break-up [s]")
fig.tight_layout(); fig.savefig(out, dpi=100)
