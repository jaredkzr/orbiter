#!/usr/bin/env python3
"""Stage times measured off a crash video's frames (30 fps): per frame, the fire's pixels
(bright, orange-to-white: R > 0.6, R > B + 0.25), the white-hot ones (all channels > 0.85),
the fire's top (highest fire row, % of the height above the horizon row) and the dark smoke's
pixels (luminance < 0.25 above the horizon).

  stages.py FRAMEDIR FIRST_FIRE_FRAME [HORIZON_FRAC]   (f_NNNNN.png; t = 0 at FIRST_FIRE_FRAME)
"""
import os, sys
import numpy as np
from PIL import Image

d, f0 = sys.argv[1], int(sys.argv[2])
hor = float(sys.argv[3]) if len(sys.argv) > 3 else 0.62
files = sorted(f for f in os.listdir(d) if f.startswith("f_") and f.endswith(".png"))
rows = []
for f in files:
    i = int(f[2:7])
    a = np.asarray(Image.open(os.path.join(d, f)).convert("RGB").resize((480, 270)), dtype=np.float32) / 255.0
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    fire = (r > 0.6) & (r > b + 0.25)
    white = (r > 0.85) & (g > 0.85) & (b > 0.75) & (r > b + 0.02)
    H = a.shape[0]; hr = int(hor * H)
    lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
    smoke = (lum[:hr] < 0.25).sum()
    ys = np.nonzero(fire.any(axis=1))[0]
    top = (hr - ys.min()) / H * 100.0 if len(ys) else 0.0
    rows.append(((i - f0) / 30.0, int(fire.sum()), int(white.sum()), top, int(smoke)))
peak = max(rows, key=lambda x: x[1])
print(f"peak fire pixels {peak[1]} at t {peak[0]:.2f} s")
for frac in (0.5, 0.1, 0.02):
    last = max((x[0] for x in rows if x[1] >= frac * peak[1]), default=None)
    print(f"fire >= {frac:.0%} of peak until t {last:.2f} s")
wp = max(rows, key=lambda x: x[2])
print(f"white-hot peak {wp[2]} px at t {wp[0]:.2f}; > 30% of it until t {max(x[0] for x in rows if x[2] >= 0.3 * wp[2]):.2f}")
print("t      fire  white  top%  smoke")
for x in rows:
    if x[0] >= -0.1 and (round(x[0] * 30) % 3 == 0 or x[0] < 1.0):
        print(f"{x[0]:5.2f} {x[1]:6d} {x[2]:6d} {x[3]:5.1f} {x[4]:6d}")
