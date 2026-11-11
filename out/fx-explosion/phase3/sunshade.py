#!/usr/bin/env python3
"""Sun-side vs shade-side crops of the ball at the same times (sunside vs sea runs): phase3/sunshade.png"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont
H = os.path.dirname(os.path.abspath(__file__))
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16)
times = json.load(open(os.path.join(H, "sea", "run.json")))["times"]
pick = [0.5, 1.2, 2.0, 3.0, 5.0]
tiles = []
for d, lab, box in (("sea", "shade side", (0.3, 0.05, 0.75, 0.6)), ("sunside", "sun side", (0.25, 0.05, 0.7, 0.6))):
    for t in pick:
        k = times.index(t)
        im = Image.open(os.path.join(H, d, "s%02d.png" % k)).convert("RGB")
        w, h = im.size
        c = im.crop((int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)))
        c = c.resize((480, round(c.height * 480 / c.width)))
        dr = ImageDraw.Draw(c); txt = "t+%g s %s" % (t, lab)
        dr.rectangle((0, 0, 9 * len(txt) + 10, 22), fill=(0, 0, 0)); dr.text((4, 2), txt, fill=(255, 255, 255), font=font)
        tiles.append(c)
cols = len(pick); th = tiles[0].height
S = Image.new("RGB", (cols * 480, 2 * th))
for i, im in enumerate(tiles): S.paste(im, ((i % cols) * 480, (i // cols) * th))
S.save(os.path.join(H, "sunshade.png")); print("ok")
