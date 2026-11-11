#!/usr/bin/env python3
"""A/B sheet of two runs per still, two pairs a row (phase 2 left, phase 3 right by default).
  ab.py SCENE [DIR_A DIR_B LABEL_A LABEL_B [OUT]]   (default: phase2/SCENE vs phase3/SCENE -> phase3/ab_SCENE.png)"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont
H = os.path.dirname(os.path.abspath(__file__))
sc = sys.argv[1]
A = sys.argv[2] if len(sys.argv) > 2 else os.path.join(H, "..", "phase2", sc)
B = sys.argv[3] if len(sys.argv) > 3 else os.path.join(H, sc)
la = sys.argv[4] if len(sys.argv) > 4 else "phase 2"
lb = sys.argv[5] if len(sys.argv) > 5 else "phase 3"
out = sys.argv[6] if len(sys.argv) > 6 else os.path.join(H, "ab_%s.png" % sc)
times = json.load(open(os.path.join(B, "run.json")))["times"]
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16)
W = 480
tiles = []
for k, t in enumerate(times):
    for d, lab in ((A, la), (B, lb)):
        p = os.path.join(d, "s%02d.png" % k)
        if not os.path.exists(p): continue
        im = Image.open(p).convert("RGB"); im = im.resize((W, round(im.height * W / im.width)))
        dr = ImageDraw.Draw(im); txt = "t+%g s %s" % (t, lab)
        dr.rectangle((0, 0, 9 * len(txt) + 10, 22), fill=(0, 0, 0)); dr.text((4, 2), txt, fill=(255, 255, 255), font=font)
        tiles.append(im)
cols = 4; th = tiles[0].height; rows = (len(tiles) + cols - 1) // cols
S = Image.new("RGB", (cols * W, rows * th))
for i, im in enumerate(tiles): S.paste(im, ((i % cols) * W, (i // cols) * th))
S.save(out)
print(out)
