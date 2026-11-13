#!/usr/bin/env python3
"""Cut the clicks (Sound/Clicks) from Freesound previews (all CC0; ids, authors and cut times in
Sound/CREDITS.md): 48 kHz mono FLAC, short fades, peak -1 dBFS.

  - switch_on_*, switch_off_*: a small toggle switch flipped one way and back (cockpit switches)
  - button_*: a push button (MFD buttons)
  - ui_*: light clicks (the launchpad's and the menus' controls)

  make_click_samples.py <dir of downloaded <id>-hq.ogg previews, saved as <id>.ogg> Sound/Clicks"""
import os
import subprocess
import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
R = 48000


def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(R), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def cut(name, sid, t0, length, fout=0.03, hp=80.0):
    x = load(f"{src}/{sid}.ogg")
    a = max(0, int((t0 - 0.003) * R))
    x = x[a:a + int(length * R)].copy()
    if hp:
        x = signal.sosfilt(signal.butter(2, hp, "highpass", fs=R, output="sos"), x)
    n, b = len(x), int(fout * R)
    x[:48] *= np.linspace(0, 1, 48) ** 2
    x[n - b:] *= np.linspace(1, 0, b) ** 2
    x *= 10 ** (-1 / 20) / np.abs(x).max()
    wav = f"{out}/{name}.wav"
    wavfile.write(wav, R, (x * 32767).astype(np.int16))
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", wav, "-c:a", "flac", "-compression_level", "8", f"{out}/{name}.flac"], check=True)
    os.remove(wav)
    print(f"{name}: {n / R * 1000:.0f} ms, RMS {20 * np.log10(np.sqrt(np.mean(x * x))):.1f} dBFS")


# a small toggle switch (877305, loganzsound): flipped one way (the stronger), then back
for k, (on, off) in enumerate([(0.609, 0.855), (1.809, 2.056), (3.009, 3.241), (5.409, 5.650)]):
    cut(f"switch_on_{k + 1}", 877305, on, 0.18)
    cut(f"switch_off_{k + 1}", 877305, off, 0.18)
# a push button (700985, DustyWind)
for k, t in enumerate([0.152, 1.628, 2.399, 3.140]):
    cut(f"button_{k + 1}", 700985, t, 0.20)
# light clicks (877181, adamdavidsmith), thinned below 300 Hz
for k, t in enumerate([1.976, 0.425, 3.782]):
    cut(f"ui_{k + 1}", 877181, t, 0.08, fout=0.02, hp=300.0)
