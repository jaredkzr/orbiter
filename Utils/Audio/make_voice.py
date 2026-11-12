#!/usr/bin/env python3
"""Make Orbiter's voice lines (Sound/Voice/*.flac) from Sound/Voice/lines.txt
with Piper TTS, offline, close to XRSound's recorded voice (a US English
female voice, about 205 Hz, -18 dBFS RMS):

  make_voice.py --piper <venv>/bin/piper --model en_US-ljspeech-high.onnx [--only <name>...]

The voice: Piper's en_US-ljspeech-high (https://huggingface.co/rhasspy/piper-voices,
models MIT; its LJ Speech dataset public domain). Each line: Piper at a
slightly brisk pace, pitched up a semitone (formants kept), the lows
under 120 Hz cut, a little presence (3 kHz), normalized to -18 dBFS RMS
(peaks under -1 dBFS), 48 kHz mono FLAC with 30 ms of silence at its start.

Needs: piper-tts (pip), ffmpeg with rubberband, numpy."""
import argparse, os, subprocess, sys, tempfile, wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
VOICE = os.path.join(ROOT, "Sound", "Voice")
RMS_DB, PEAK_DB = -18.0, -1.0

def read_wav(path):
    with wave.open(path) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
        return x.reshape(-1, w.getnchannels()).mean(axis=1), w.getframerate()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--piper", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--pitch", type=float, default=2 ** (1 / 12))
    a = ap.parse_args()
    lines = []
    for l in open(os.path.join(VOICE, "lines.txt"), encoding="utf-8"):
        l = l.strip()
        if l and not l.startswith("#"):
            name, text = l.split("|", 1)
            lines.append((name.strip(), text.strip()))
    with tempfile.TemporaryDirectory() as tmp:
        for name, text in lines:
            if a.only and name not in a.only:
                continue
            raw, shaped = os.path.join(tmp, "raw.wav"), os.path.join(tmp, "shaped.wav")
            subprocess.run([a.piper, "-m", a.model, "-f", raw, "--length-scale", "0.92", "--sentence-silence", "0"],
                           input=text.encode(), check=True, stderr=subprocess.DEVNULL)
            af = (f"rubberband=pitch={a.pitch:.5f}:formant=preserved,highpass=f=120,"
                  "equalizer=f=3000:t=q:w=1.0:g=3,aresample=48000,"
                  "silenceremove=start_periods=1:start_threshold=-50dB,adelay=30,apad=pad_dur=0.05")
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", raw, "-af", af, "-ac", "1", "-c:a", "pcm_s16le", shaped], check=True)
            x, sr = read_wav(shaped)
            rms = np.sqrt(np.mean(x[np.abs(x) > 1e-3] ** 2))   # (the speech, not the pauses)
            g = min(10 ** (RMS_DB / 20) / rms, 10 ** (PEAK_DB / 20) / np.abs(x).max())
            out = os.path.join(VOICE, name + ".flac")
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", shaped, "-af", f"volume={g:.6f}",
                            "-c:a", "flac", "-sample_fmt", "s16", out], check=True)
            print(f"{name}.flac: {len(x) / sr:.2f} s, gain {20 * np.log10(g):+.1f} dB  \"{text}\"")

if __name__ == "__main__":
    sys.exit(main())
