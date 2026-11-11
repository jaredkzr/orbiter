#!/usr/bin/env python3
"""Pause check: the three frames taken 4 frames apart while paused (run.py --pausecheck) compared:
mean and max absolute difference (0..255) over the frame and over the fireball's box (the
brightest region); a strip sheet. pausediff.py RUNDIR OUT.png"""
import os, sys
import numpy as np
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
ims = [np.asarray(Image.open(os.path.join(d, f"pause_{k}.png")).convert("RGB"), dtype=np.float64) for k in range(3)]
for k in (1, 2):
    diff = np.abs(ims[k] - ims[0]).mean(axis=2)
    print(f"pause_{k} vs pause_0: mean |diff| {diff.mean():.3f}, max {diff.max():.1f}, pixels over 8: {(diff > 8).sum()} of {diff.size}")
S = Image.new("RGB", (3 * ims[0].shape[1] // 2, ims[0].shape[0] // 2))
for k in range(3):
    im = Image.fromarray(ims[k].astype(np.uint8)).resize((ims[0].shape[1] // 2, ims[0].shape[0] // 2))
    S.paste(im, (k * im.width, 0))
S.save(out)
