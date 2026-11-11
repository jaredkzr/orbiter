#!/usr/bin/env python3
"""A frame strip of a run.py --video run at 0.1 s intervals from -0.1 s (6 x 4, as the reviewer's
phase3/anim/sea_anim.png), cropped about the ball. strip.py RUNDIR OUT.png [CROP x0,y0,x1,y1 (fractions)]"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont
d, out = sys.argv[1], sys.argv[2]
crop = tuple(float(x) for x in sys.argv[3].split(",")) if len(sys.argv) > 3 else (0.25, 0.1, 0.75, 0.6)
rj = json.load(open(os.path.join(d, "run.json"))); npre = int(round(rj["video"][0] * 30))
vid = os.path.join(d, "video"); fs = sorted(f for f in os.listdir(vid) if f.startswith("f_"))
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16)
tiles = []
for k in range(24):
    t = -0.1 + 0.1 * k
    i = npre + int(round(t * 30))
    if i >= len(fs) or i < 0: break
    im = Image.open(os.path.join(vid, fs[i])).convert("RGB"); w, h = im.size
    c = im.crop((int(w * crop[0]), int(h * crop[1]), int(w * crop[2]), int(h * crop[3])))
    c = c.resize((450, round(c.height * 450 / c.width)))
    dr = ImageDraw.Draw(c); lab = "%+.1f s" % t
    dr.rectangle((0, 0, 9 * len(lab) + 10, 22), fill=(0, 0, 0)); dr.text((4, 2), lab, fill=(255, 255, 255), font=font)
    tiles.append(c)
cols = 6; rows = (len(tiles) + cols - 1) // cols; W, H = tiles[0].size
S = Image.new("RGB", (cols * W, rows * H))
for i, im in enumerate(tiles): S.paste(im, ((i % cols) * W, (i // cols) * H))
S.save(out); print(out)
