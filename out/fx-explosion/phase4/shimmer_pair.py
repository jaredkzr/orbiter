#!/usr/bin/env python3
"""Close-up crop pairs of the heat shimmer: the 'shimmer' scene with the shimmer off (left) and on
(right) at the fireball's right rim (the sea behind it) and over the column above it, plus the
difference image. shimmer_pair.py DIR_OFF DIR_ON OUT.png"""
import sys, numpy as np
from PIL import Image, ImageDraw, ImageFont
off, on, out = sys.argv[1:4]
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 18)
rows = []
for k, t, box in ((0, 0.5, (0.56, 0.33, 0.86, 0.63)), (1, 0.8, (0.56, 0.3, 0.86, 0.6)), (2, 1.2, (0.3, 0.05, 0.7, 0.35))):
    a = Image.open("%s/s%02d.png" % (off, k)).convert("RGB"); b = Image.open("%s/s%02d.png" % (on, k)).convert("RGB")
    w, h = a.size; bx = (int(w * box[0]), int(h * box[1]), int(w * box[2]), int(h * box[3]))
    ca, cb = a.crop(bx), b.crop(bx)
    d = np.abs(np.asarray(ca, dtype=float) - np.asarray(cb, dtype=float)).mean(axis=2)
    cd = Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).convert("RGB")
    row = Image.new("RGB", (3 * ca.width, ca.height))
    for i, (im, lab) in enumerate(((ca, "t+%g s shimmer off" % t), (cb, "t+%g s shimmer on" % t), (cd, "difference x4"))):
        im = im.copy(); dr = ImageDraw.Draw(im); dr.rectangle((0, 0, 10 * len(lab) + 10, 26), fill=(0, 0, 0)); dr.text((5, 3), lab, fill=(255, 255, 255), font=font)
        row.paste(im, (i * ca.width, 0))
    rows.append(row)
W = max(r.width for r in rows); S = Image.new("RGB", (W, sum(r.height for r in rows))); y = 0
for r in rows: S.paste(r, (0, y)); y += r.height
S.save(out); print(out)
