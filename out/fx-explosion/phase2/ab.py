#!/usr/bin/env python3
"""A/B sheet: sprites (left) vs volume (right) per still, two pairs a row.
  ab.py SCENE  (phase2/SCENE_sprites, phase2/SCENE -> phase2/ab_SCENE.png)"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont
H = os.path.dirname(os.path.abspath(__file__))
sc = sys.argv[1]
A, B = os.path.join(H, sc + "_sprites"), os.path.join(H, sc)
times = json.load(open(os.path.join(B, "run.json")))["times"]
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16)
W = 480
tiles = []
for k, t in enumerate(times):
    for d, lab in ((A, "sprites"), (B, "volume")):
        p = os.path.join(d, "s%02d.png" % k)
        if not os.path.exists(p): continue
        im = Image.open(p).convert("RGB"); im = im.resize((W, round(im.height * W / im.width)))
        dr = ImageDraw.Draw(im); txt = "t+%g s %s" % (t, lab)
        dr.rectangle((0, 0, 9 * len(txt) + 10, 22), fill=(0, 0, 0)); dr.text((4, 2), txt, fill=(255, 255, 255), font=font)
        tiles.append(im)
cols = 4; th = tiles[0].height; rows = (len(tiles) + cols - 1) // cols
S = Image.new("RGB", (cols * W, rows * th))
for i, im in enumerate(tiles): S.paste(im, ((i % cols) * W, (i // cols) * th))
S.save(os.path.join(H, "ab_%s.png" % sc))
