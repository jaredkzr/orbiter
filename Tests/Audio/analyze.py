#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""Analysis of Orbiter's audio renders (ORBITER_AUDIO_RENDER=<file.wav>).

A summary of the render (duration, peak, level, DC, clipping, clicks), its
levels over time in frequency bands, and a spectrogram picture; checks on
all of these for the tests (non-zero exit when one fails).

  analyze.py render.wav                       summary
  analyze.py render.wav --png spec.png        and a spectrogram
  analyze.py render.wav --duration 2.0 --silent
  analyze.py render.wav --level 1.0:3.0:all:-30:-6
        (from 1 s to 3 s, all frequencies, the level between -30 and -6 dBFS)
  analyze.py render.wav --level 5:6:low:-inf:-60   (silence below 200 Hz)

Bands: all, low (20-200 Hz), mid (200-2000 Hz), high (2-20 kHz), or
<f0>-<f1> in Hz. Levels: RMS of both channels, in dBFS (a full-scale sine
is -3 dBFS).
"""

import argparse
import json
import math
import sys

import numpy as np
from scipy.io import wavfile
from scipy import signal

BANDS = {"all": (0.0, None), "low": (20.0, 200.0), "mid": (200.0, 2000.0), "high": (2000.0, 20000.0)}
SILENCE_DB = -90.0


def load(path):
    rate, data = wavfile.read(path)
    if data.dtype.kind == "i":
        data = data.astype(np.float64) / np.iinfo(data.dtype).max
    else:
        data = data.astype(np.float64)
    if data.ndim == 1:
        data = data[:, None]
    return rate, data


def db(x):
    return 20.0 * math.log10(x) if x > 0 else -math.inf


def band_limits(name, rate):
    if name in BANDS:
        lo, hi = BANDS[name]
    else:
        lo, hi = (float(v) for v in name.split("-"))
    nyq = rate / 2
    if hi is None or hi >= nyq:
        hi = None
    return lo, hi


def band_filter(data, rate, name):
    lo, hi = band_limits(name, rate)
    if lo <= 0 and hi is None:
        return data
    if lo <= 0:
        sos = signal.butter(4, hi, "lowpass", fs=rate, output="sos")
    elif hi is None:
        sos = signal.butter(4, lo, "highpass", fs=rate, output="sos")
    else:
        sos = signal.butter(4, [lo, hi], "bandpass", fs=rate, output="sos")
    return signal.sosfilt(sos, data, axis=0)


def rms_db(data):
    if len(data) == 0:
        return -math.inf
    return db(math.sqrt(float(np.mean(data * data))))


def clicks(data, rate):
    """Sample-to-sample jumps far above what the local signal does: the
    clicks of a discontinuity (a sound cut off, a buffer gap)."""
    d = np.abs(np.diff(data, axis=0)).max(axis=1)
    if len(d) < 64:
        return 0
    win = max(1, rate // 200)  # 5 ms
    local = np.sqrt(np.convolve(d * d, np.ones(win) / win, mode="same"))
    jumps = (d > 0.05) & (d > 8.0 * local)
    return int(np.count_nonzero(jumps))


def summary(rate, data):
    peak = float(np.abs(data).max()) if data.size else 0.0
    return {
        "rate": int(rate),
        "channels": int(data.shape[1]),
        "duration": len(data) / rate,
        "peak_db": db(peak),
        "rms_db": rms_db(data),
        "dc": float(np.abs(data.mean(axis=0)).max()) if data.size else 0.0,
        "clipped": int(np.count_nonzero(np.abs(data) >= 1.0)),
        "clicks": clicks(data, rate),
        "bands_db": {b: rms_db(band_filter(data, rate, b)) for b in ("low", "mid", "high")},
    }


def level(rate, data, t0, t1, band):
    i0, i1 = int(max(0.0, t0) * rate), int(min(len(data) / rate, t1) * rate)
    # (filtered from a little before the window: the filter settles)
    pre = int(0.1 * rate)
    seg = band_filter(data[max(0, i0 - pre):i1], rate, band)
    return rms_db(seg[i0 - max(0, i0 - pre):])


def envelope(rate, data, band="all", window=0.05):
    """Level over time [(t, dB)] in windows of `window` seconds"""
    f = band_filter(data, rate, band)
    n = max(1, int(window * rate))
    out = []
    for i in range(0, len(f) - n + 1, n):
        out.append((i / rate, rms_db(f[i:i + n])))
    return out


def spectrogram_png(rate, data, path, title=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    mono = data.mean(axis=1)
    fig, (ax0, ax1) = plt.subplots(2, 1, figsize=(12, 7), sharex=True, gridspec_kw={"height_ratios": [1, 3]})
    t = np.arange(len(mono)) / rate
    env = envelope(rate, data)
    ax0.plot([e[0] for e in env], [max(e[1], -100) for e in env], color="#3b82f6", lw=1)
    for b, c in (("low", "#ef4444"), ("high", "#10b981")):
        e = envelope(rate, data, b)
        ax0.plot([x[0] for x in e], [max(x[1], -100) for x in e], color=c, lw=0.8, label=b)
    ax0.set_ylabel("dBFS")
    ax0.set_ylim(-100, 0)
    ax0.grid(alpha=0.3)
    ax0.legend(loc="upper right", fontsize=8)
    if len(mono) > 1024:
        f, tt, sxx = signal.spectrogram(mono, fs=rate, nperseg=2048, noverlap=1536)
        sxx = 10 * np.log10(sxx + 1e-14)
        ax1.pcolormesh(tt, f, sxx, shading="auto", vmin=-140, vmax=-20, cmap="magma")
        ax1.set_yscale("symlog", linthresh=100)
        ax1.set_ylim(20, rate / 2)
    ax1.set_ylabel("Hz")
    ax1.set_xlabel("s")
    if title:
        fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--png", help="write a spectrogram picture")
    ap.add_argument("--json", action="store_true", help="the summary as JSON")
    ap.add_argument("--duration", type=float, help="expected duration [s] (within one 50 Hz step)")
    ap.add_argument("--silent", action="store_true", help="expect silence throughout")
    ap.add_argument("--no-clip", action="store_true", help="expect no samples at or beyond full scale")
    ap.add_argument("--no-clicks", action="store_true", help="expect no clicks")
    ap.add_argument("--level", action="append", default=[], metavar="T0:T1:BAND:MIN:MAX",
                    help="expect the level from T0 to T1 [s] in BAND between MIN and MAX dBFS")
    a = ap.parse_args(argv)

    rate, data = load(a.wav)
    s = summary(rate, data)
    if a.json:
        print(json.dumps(s, indent=1))
    else:
        print(f"{a.wav}: {s['duration']:.3f} s, {s['rate']} Hz, {s['channels']} ch, peak {s['peak_db']:.1f} dBFS, "
              f"RMS {s['rms_db']:.1f} dBFS, DC {s['dc']:.2g}, clipped {s['clipped']}, clicks {s['clicks']}")
        print("  bands: " + ", ".join(f"{b} {v:.1f} dB" for b, v in s["bands_db"].items()))
    if a.png:
        spectrogram_png(rate, data, a.png, a.wav)

    failed = []
    if a.duration is not None and abs(s["duration"] - a.duration) > 0.02 + 1.0 / rate:
        failed.append(f"duration {s['duration']:.3f} s, expected {a.duration:.3f} s")
    if a.silent and s["peak_db"] > SILENCE_DB:
        failed.append(f"not silent: peak {s['peak_db']:.1f} dBFS")
    if a.no_clip and s["clipped"]:
        failed.append(f"{s['clipped']} samples clipped")
    if a.no_clicks and s["clicks"]:
        failed.append(f"{s['clicks']} clicks")
    for spec in a.level:
        t0, t1, band, lo, hi = spec.split(":")
        v = level(rate, data, float(t0), float(t1), band)
        ok = float(lo) <= v <= float(hi)
        print(f"  level {t0}-{t1} s {band}: {v:.1f} dBFS (expected {lo}..{hi}) {'ok' if ok else 'FAILED'}")
        if not ok:
            failed.append(f"level {spec}: {v:.1f} dBFS")
    for f in failed:
        print("FAILED: " + f)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
