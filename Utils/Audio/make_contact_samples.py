#!/usr/bin/env python3
"""Cut the contact one-shots and loops (Sound/Contacts) from Freesound previews (all CC0; ids, authors and cut times in
Sound/CREDITS.md): 48 kHz mono FLAC, fades, normalized.

  make_contact_samples.py <dir of downloaded <id>.mp3 previews> Sound/Contacts"""
import subprocess, numpy as np, os, sys
from scipy.io import wavfile
src = sys.argv[1]; out = sys.argv[2]; os.makedirs(out, exist_ok=True)
R = 48000
def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(R), "-f", "f32le", "-"], capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)
def cut(name, sid, t0, t1, fin=0.004, fout=0.05, peak=None, rmsdb=None, hp=None):
    x = load(f"{src}/{sid}.mp3")[int(t0 * R):int(t1 * R)].copy()
    if hp:
        from scipy import signal
        sos = signal.butter(2, hp, "highpass", fs=R, output="sos"); x = signal.sosfilt(sos, x)
    n = len(x); a = int(fin * R); b = int(fout * R)
    x[:a] *= np.linspace(0, 1, a) ** 2; x[n - b:] *= np.linspace(1, 0, b) ** 2
    if peak is not None: x *= 10 ** (peak / 20) / np.abs(x).max()
    if rmsdb is not None: x *= 10 ** (rmsdb / 20) / np.sqrt(np.mean(x * x))
    x = np.clip(x, -1, 1)
    wav = f"{out}/{name}.wav"; wavfile.write(wav, R, (x * 32767).astype(np.int16))
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", wav, "-c:a", "flac", "-compression_level", "8", f"{out}/{name}.flac"], check=True)
    os.remove(wav)
    print(f"{name}: {n / R:.2f} s peak {20*np.log10(np.abs(x).max()):.1f} dB RMS {20*np.log10(np.sqrt(np.mean(x*x))):.1f} dB")
# one-shots: peak -1 dBFS
cut("clunk_heavy", 275441, 2.400, 3.50, fin=0.002, peak=-1)      # a heavy steel door slamming: the slam, its rattle
cut("thump_door", 173009, 0.30, 0.76, peak=-1)       # a car door closing: a low thump
cut("door_close", 208695, 0.10, 0.57, peak=-1)       # a car door closing: thump and its latch's click
cut("bump", 9876, 0.00, 0.55, peak=-1)               # a car door closed: a dull bump
cut("latch_1", 331138, 0.02, 0.50, peak=-1)          # a metal door's lock: a punchy clack with a body
cut("latch_2", 119494, 0.81, 1.20, peak=-1)          # a slide lock's bolt home
cut("latch_3", 481789, 0.17, 0.42, peak=-1)          # a metallic latch releasing
cut("lock", 396448, 0.04, 0.20, peak=-1)             # a car's central lock: a short motor clack
# loops: a set RMS
cut("squeal", 536769, 0.40, 1.45, fin=0.02, fout=0.02, rmsdb=-12, hp=450)   # a tyre's squeal (steady part)
cut("scrape", 543625, 0.20, 3.60, fin=0.02, fout=0.02, rmsdb=-12)   # metal dragged
cut("motor", 125401, 2.00, 6.00, fin=0.02, fout=0.02, rmsdb=-12, hp=60)    # an electric motor running
