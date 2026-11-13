#!/usr/bin/env python3
"""Make Orbiter's head-related transfer functions (Sound/HRTF/kemar.hrtf)
from the MIT Media Lab's KEMAR measurements (Gardner & Martin, 1994: the
"compact" set - 128-tap stereo impulse responses at 44.1 kHz, 368
directions on the right, elevations -40..90 degrees; free to use, the
authors cited: see Sound/CREDITS.md).

  - the full sphere: the left side mirrored from the right
  - diffuse-field equalized: the mean over all directions taken out (the
    dummy head's ear canal, the measuring system), flat below 200 Hz (the
    head is small beside such waves), within +-15 dB
  - each ear's arrival (the interaural delay) taken apart from its filter,
    the filter made minimum phase and cut to 64 taps at 48 kHz
  - the level: the mean over the sphere of both ears' power 1 (equal-power
    panning's: the mix keeps its level with it)

Usage: make_hrtf.py [--zip compact.zip] [--out Sound/HRTF/kemar.hrtf]
(without --zip: downloaded from sound.media.mit.edu)

The file (little-endian):
  "OHRTF1\\0\\0"; uint32 rate, taps, count; then count times:
  float elevation, azimuth [deg] (azimuth clockwise from ahead: 90 right),
  float delayL, delayR [samples], float left[taps], right[taps]
"""

import argparse
import io
import os
import re
import struct
import sys
import tempfile
import urllib.request
import wave
import zipfile

import numpy as np
from scipy.signal import resample_poly

URL = "https://sound.media.mit.edu/resources/KEMAR/compact.zip"
RATE_IN, RATE = 44100, 48000
TAPS = 64
NFFT = 1024


def load(zpath):
    z = zipfile.ZipFile(zpath)
    out = []
    for name in z.namelist():
        m = re.search(r"H(-?\d+)e(\d+)a\.wav$", name)
        if not m:
            continue
        el, az = int(m.group(1)), int(m.group(2))
        w = wave.open(io.BytesIO(z.read(name)))
        assert w.getframerate() == RATE_IN and w.getnchannels() == 2
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2).astype(float) / 32768.0
        out.append((el, az, x[:, 0], x[:, 1]))
    return out


def sphere(data):
    """The full sphere: (el, az, left, right), az mirrored to the left"""
    full = []
    for el, az, l, r in data:
        full.append((el, float(az), l, r))
        if 0 < az < 180 and el != 90:
            full.append((el, float(360 - az), r, l))
    full.sort(key=lambda d: (d[0], d[1]))
    return full


def onset(h, frac=0.15, up=8):
    """The arrival [samples]: where it first reaches frac of its peak"""
    u = resample_poly(h, up, 1)
    a = np.abs(u)
    i = int(np.argmax(a >= frac * a.max()))
    return i / up


def minimum_phase(mag):
    """A minimum-phase impulse response with this magnitude (full FFT bins)"""
    n = len(mag)
    logm = np.log(np.maximum(mag, 1e-6))
    c = np.fft.ifft(logm).real
    w = np.zeros(n)
    w[0] = 1.0
    w[1:n // 2] = 2.0
    w[n // 2] = 1.0
    return np.fft.ifft(np.exp(np.fft.fft(c * w))).real


def smooth_octave(p, freqs, frac=3.0):
    """Power spectrum smoothed over 1/frac octave"""
    out = np.empty_like(p)
    for i, f in enumerate(freqs):
        if f <= 0:
            out[i] = p[i]
            continue
        lo, hi = f * 2 ** (-0.5 / frac), f * 2 ** (0.5 / frac)
        sel = (freqs >= lo) & (freqs <= hi)
        out[i] = p[sel].mean()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "..", "Sound", "HRTF", "kemar.hrtf"))
    a = ap.parse_args()
    zpath = a.zip
    if not zpath:
        zpath = os.path.join(tempfile.mkdtemp(), "compact.zip")
        urllib.request.urlretrieve(URL, zpath)
    full = sphere(load(zpath))

    # 48 kHz
    res = []
    for el, az, l, r in full:
        res.append((el, az, resample_poly(l, RATE // 300, RATE_IN // 300), resample_poly(r, RATE // 300, RATE_IN // 300)))

    # each ear's arrival, the earliest of the pair at a common origin
    freqs = np.fft.fftfreq(NFFT, 1.0 / RATE)
    pos = np.abs(freqs)
    specs, delays, weights = [], [], []
    rings = {}
    for el, az, l, r in res:
        rings[el] = rings.get(el, 0) + 1
    for el, az, l, r in res:
        dl, dr = onset(l), onset(r)
        delays.append((dl, dr))
        specs.append((np.abs(np.fft.fft(l, NFFT)), np.abs(np.fft.fft(r, NFFT))))
        # (solid angle: a ring's share by the cosine of its elevation)
        weights.append(max(np.cos(np.radians(el)), 0.05) / rings[el])
    weights = np.array(weights) / np.sum(weights)
    base = min(min(d) for d in delays)
    delays = [(dl - base, dr - base) for dl, dr in delays]

    # the diffuse field: the mean power over the sphere and both ears
    df = np.zeros(NFFT)
    for (ml, mr), w in zip(specs, weights):
        df += w * 0.5 * (ml ** 2 + mr ** 2)
    df = smooth_octave(df, pos)
    eq = 1.0 / np.sqrt(df)
    eq_db = 20 * np.log10(eq)
    eq_db -= np.interp(1000.0, pos[:NFFT // 2], eq_db[:NFFT // 2]) # (0 dB at 1 kHz)
    eq_db = np.clip(eq_db, -15.0, 15.0)
    eq = 10 ** (eq_db / 20)

    out = []
    i200 = int(round(200.0 * NFFT / RATE))
    for (el, az, _, _), (ml, mr), (dl, dr) in zip(res, specs, delays):
        irs = []
        for m in (ml, mr):
            mag = m * eq
            # (flat below 200 Hz: at its level there)
            mag = np.where(pos < 200.0, mag[i200], mag)
            h = minimum_phase(mag)[:TAPS].copy()
            h[-16:] *= 0.5 + 0.5 * np.cos(np.linspace(0, np.pi, 16))
            irs.append(h)
        out.append((el, az, dl, dr, irs[0], irs[1]))

    # the level: both ears' power, over the sphere, 1
    p = sum(w * (np.sum(o[4] ** 2) + np.sum(o[5] ** 2)) for o, w in zip(out, weights))
    g = 1.0 / np.sqrt(p)
    out = [(el, az, dl, dr, l * g, r * g) for el, az, dl, dr, l, r in out]

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(b"OHRTF1\0\0")
        f.write(struct.pack("<III", RATE, TAPS, len(out)))
        for el, az, dl, dr, l, r in out:
            f.write(struct.pack("<4f", el, az, dl, dr))
            f.write(np.asarray(l, "<f4").tobytes())
            f.write(np.asarray(r, "<f4").tobytes())

    # a readout
    def find(el, az):
        return min(out, key=lambda o: abs(o[0] - el) + abs(((o[1] - az + 180) % 360) - 180))
    for el, az in ((0, 0), (0, 90), (0, 270), (0, 180), (90, 0), (-40, 0)):
        o = find(el, az)
        pl, pr = np.sum(o[4] ** 2), np.sum(o[5] ** 2)
        print(f"el {el:4d} az {az:4d}: L {10*np.log10(pl):6.1f} dB  R {10*np.log10(pr):6.1f} dB  ITD {(o[3]-o[2])/RATE*1e6:7.1f} us")
    print(f"{len(out)} directions, {TAPS} taps at {RATE} Hz -> {a.out} ({os.path.getsize(a.out)} bytes)")


if __name__ == "__main__":
    main()
