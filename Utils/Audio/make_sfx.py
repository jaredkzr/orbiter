#!/usr/bin/env python3
"""Make Orbiter's generated sound effects (Sound/Generated/*.flac) offline from
Sound/Generated/prompts.txt, with MOSS-SoundEffect v2.0 (OpenMOSS, Apache-2.0):

  make_sfx.py [--prompts Sound/Generated/prompts.txt] [--only <name>...] [--force]
              [--cache ~/.cache/orbiter-sfx]

Run it with a Python that has torch and the MOSS-SoundEffect pipeline (moss_soundeffect_v2;
on ROCm: TORCHDYNAMO_DISABLE=1). Each line: "<name>|<prompt>|<seconds>|<seed>[|<post>]".
The model always renders a fixed window and is told the duration in its prompt ("<prompt>
duration: <seconds>s"); the same prompt, seconds and seed give the same sound, so a line
reproduces its file. Names starting with "_" are only parts (a layer of another): not written.

A prompt "tone:<segment>,..." is synthesized instead (a cue: the caution chirp, the master alarm):
each segment "<Hz>[-<Hz>]/<s>[/d]" - a tone (or a sweep between two), "0/<s>" silence, "/d" decaying like
a chime; 5 ms ramps, two soft harmonics, a small speaker's band (300 Hz to 4 kHz). <seconds> and <seed>
are ignored for these.

<post>, comma-separated, in this order:
  keep=<s>          keep this much of the render (conditioned on <seconds>: the same take, a longer tail)
  trim=<a>-<b>      keep <a> to <b> seconds
  reverse           played backwards (a spool-down from a spool-up: its exact mirror)
  hp=<Hz>           high-pass (12 dB/octave)
  layer=<name>@<dB> mix in another line's sound, its loud moments (the 99th percentile of its 20 ms
                    level) <dB> under this one's
  fade=<in>/<out>   fades [s] (the out shaped: it drops faster)
  loop=<s>          a seamless loop: its last <s> crossfaded (equal power) into its start
  level=<dBFS>      the RMS of its sound (the frames within 40 dB of its loudest) - the level the
                    code expects (Ambience.cpp: CABIN_RMS, EVENT_RMS); default -24
48 kHz mono 16-bit FLAC. Raw renders are cached (--cache), keyed by prompt, seconds, keep and seed."""
import argparse, hashlib, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
GEN = os.path.join(ROOT, "Sound", "Generated")
SR = 48000
STEPS, CFG = 100, 4.0


def read_prompts(path):
    out = {}
    for l in open(path, encoding="utf-8"):
        l = l.strip()
        if l and not l.startswith("#"):
            f = [s.strip() for s in l.split("|")]
            post = []
            if len(f) > 4 and f[4]:
                for op in f[4].split(","):
                    k, _, v = op.partition("=")
                    post.append((k.strip(), v.strip()))
            out[f[0]] = {"prompt": f[1], "seconds": float(f[2]), "seed": int(f[3]), "post": post}
    return out


def envelope(x, frame_s=0.02):
    f = max(1, int(frame_s * SR))
    n = len(x) // f
    return np.sqrt((x[: n * f].reshape(n, f) ** 2).mean(axis=1))


def highpass(x, hz):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    return np.fft.irfft(X / np.sqrt(1 + (hz / np.maximum(f, 1e-3)) ** 4), len(x))


def fade(x, fin, fout):
    x = x.copy()
    a, b = int(fin * SR), int(fout * SR)
    if a:
        x[:a] *= np.linspace(0, 1, a)
    if b:
        x[-b:] *= np.linspace(1, 0, b) ** 1.6
    return x


def loop(x, xf):
    """the last xf seconds crossfaded into the start: x[n] flows on into x[0]"""
    n = int(xf * SR)
    t = np.linspace(0, np.pi / 2, n)
    head = x[:n] * np.sin(t) + x[-n:] * np.cos(t)
    return np.concatenate([head, x[n:-n]])


def level(x, dbfs):
    env = envelope(x)
    loud = np.repeat(env > env.max() * 10 ** (-40 / 20), int(0.02 * SR))[: len(x)]
    a = x[: len(loud)][loud] if loud.any() else x
    g = 10 ** (dbfs / 20) / max(np.sqrt(np.mean(a ** 2)), 1e-12)
    return x * min(g, 10 ** (-1 / 20) / max(np.abs(x).max(), 1e-12))     # (peaks under -1 dBFS)


def tone(spec):
    """a synthesized cue (see the module's doc)"""
    out = []
    for seg in spec.split(","):
        parts = seg.strip().split("/")
        f, dur = parts[0], float(parts[1])
        decay = len(parts) > 2 and parts[2] == "d"
        n = int(dur * SR)
        t = np.arange(n) / SR
        f0, _, f1 = f.partition("-")
        f0 = float(f0); f1 = float(f1) if f1 else f0
        if f0 <= 0:
            out.append(np.zeros(n)); continue
        ph = 2 * np.pi * np.cumsum(np.linspace(f0, f1, n)) / SR
        x = np.sin(ph) + 0.3 * np.sin(2 * ph) + 0.12 * np.sin(3 * ph)
        r = min(int(0.005 * SR), n // 2)
        env = np.ones(n)
        env[:r] = np.linspace(0, 1, r)
        env[-r:] = np.linspace(1, 0, r)
        if decay:
            env *= np.exp(-t / (dur / 4))
        out.append(x * env)
    x = np.concatenate(out)
    X = np.fft.rfft(x)
    fr = np.fft.rfftfreq(len(x), 1 / SR)
    X /= np.sqrt(1 + (300 / np.maximum(fr, 1e-3)) ** 4) * np.sqrt(1 + (fr / 4000) ** 4)   # (a small speaker)
    return np.fft.irfft(X, len(x))


class Renderer:
    def __init__(self, cache):
        self.cache, self.pipe = cache, None
        os.makedirs(cache, exist_ok=True)

    def raw(self, prompt, seconds, seed, keep):
        import soundfile as sf
        key = hashlib.sha1(f"{prompt}|{seconds}|{keep}|{seed}|{STEPS}|{CFG}".encode()).hexdigest()[:16]
        path = os.path.join(self.cache, key + ".wav")
        if os.path.exists(path):
            return sf.read(path, dtype="float64")[0]
        if self.pipe is None:
            import torch
            from moss_soundeffect_v2 import MossSoundEffectPipeline
            # (gfx1201: the fused attention kernels fail; the math path)
            torch.backends.cuda.enable_flash_sdp(False)
            torch.backends.cuda.enable_mem_efficient_sdp(False)
            torch.backends.cuda.enable_math_sdp(True)
            self.pipe = MossSoundEffectPipeline.from_pretrained("OpenMOSS-Team/MOSS-SoundEffect-v2.0", torch_dtype=torch.bfloat16, device="cuda")
            self.pipe.engine.vram_management_enabled = True   # (16 GiB: one stage's model on the GPU at a time)
        audio = self.pipe(prompt=f"{prompt} duration: {seconds:.1f}s", seconds=keep, append_duration_suffix=False,
                          num_inference_steps=STEPS, cfg_scale=CFG, seed=seed)
        x = audio[0].float().cpu().numpy().T.mean(axis=1)[: int(keep * SR)].astype(np.float64)
        sf.write(path, x.astype(np.float32), SR, subtype="FLOAT")
        return x


def make(name, P, R, done):
    if name in done:
        return done[name]
    p = P[name]
    post = dict(p["post"])
    keep = float(post.get("keep", p["seconds"]))
    x = tone(p["prompt"][5:]) if p["prompt"].startswith("tone:") else R.raw(p["prompt"], p["seconds"], p["seed"], keep)
    for k, v in p["post"]:
        if k == "trim":
            a, b = (float(s) for s in v.split("-"))
            x = x[int(a * SR): int(b * SR)]
        elif k == "reverse":
            x = x[::-1].copy()
        elif k == "hp":
            x = highpass(x, float(v))
        elif k == "layer":
            other, _, db = v.partition("@")
            y = make(other, P, R, done)[: len(x)]
            y = np.pad(y, (0, len(x) - len(y)))
            loud = lambda z: np.percentile(envelope(z), 99)
            x = x + y * loud(x) * 10 ** (float(db) / 20) / max(loud(y), 1e-12)
        elif k == "fade":
            a, b = (float(s) for s in v.split("/"))
            x = fade(x, a, b)
        elif k == "loop":
            x = loop(x, float(v))
    x = level(x, float(post.get("level", -24.0)))
    done[name] = x
    return x


def main():
    import soundfile as sf
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--prompts", default=os.path.join(GEN, "prompts.txt"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--force", action="store_true", help="remake files that exist")
    ap.add_argument("--cache", default=os.path.expanduser("~/.cache/orbiter-sfx"))
    a = ap.parse_args()
    P = read_prompts(a.prompts)
    outdir = os.path.dirname(os.path.abspath(a.prompts))
    R, done = Renderer(a.cache), {}
    for name in P:
        if name.startswith("_") or (a.only and name not in a.only):
            continue
        out = os.path.join(outdir, name + ".flac")
        if os.path.exists(out) and not a.force:
            continue
        x = make(name, P, R, done)
        sf.write(out, np.clip(x, -1, 1), SR, subtype="PCM_16")
        print(f"{name}.flac: {len(x) / SR:.2f} s, peak {20 * np.log10(np.abs(x).max() + 1e-12):.1f} dBFS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
