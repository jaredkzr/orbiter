#!/usr/bin/env python3
"""Make Orbiter's voice lines offline, from a lines file ("<file name>|<text>[|<voice>[|<takes>[|<bed>]]]").

Qwen3-TTS (the cast; run it with a Python that has torch, qwen-tts and faster-whisper):
  make_voice.py --engine qwen3 [--voice crew] [--lines Sound/Voice/lines.txt] [--only <name>...]
                [--reroll] [--force]
    Each line is said by a voice cloned from its reference (Utils/Audio/voices/<voice>.flac,
    transcript in voices.txt), with Qwen3-TTS-12Hz-1.7B-Base (Apache-2.0). Seeds are tried in
    order and the first take Whisper (faster-whisper small.en) accepts is kept: it rejects a
    take that is off script, cut off, far too long or short for its words, or clipped. Whisper
    never ranks takes; a person listens through the kept ones and rerolls what needs it.
    Kept seeds go to <lines>.takes.json, so --force rebuilds the same files (same seed, same
    audio on the same model). Lines with takes > 1 (Mission Control) make <name>_<n>.flac.
    --only <name>... just those lines; --reroll (with --only) drops their kept takes and goes on
    to new seeds; --force remakes lines that already have a file (from their kept seeds).
    Rework: --candidates N --review DIR (with --only) makes N new accepted takes of each line
    into DIR (<name>__s<seed>.flac, DIR/candidates.json) and leaves Sound/Voice alone; then
    --keep "<name>=<seed>"... makes the chosen ones the kept takes.
    Shaping: pauses over 0.70 s cut to 0.55 s, silence trimmed, the lows under 120 Hz cut,
    -18 dBFS RMS (peaks under -1 dBFS), 48 kHz mono FLAC with 30 ms of silence at its start.
    --min-voice-sim <cos> also rejects a take that drifts into another voice (Qwen's own speaker
    embedding of the take against the reference's).
    --max-rise <semitones> also rejects a take whose pitch rises at its end (the last 150 ms of
    voice against the 400 ms before): a statement said like a question.
    <bed> "<sound file>@[<from>-]<to>[,<dB>/<s>]": a sound with a spoken line at its end
    (XRSound's Docking.wav: a latch, then "ship is docked"): it plays from <from> (default 0,
    faded in) to <to>, then the new line; its first <s> seconds <dB> quieter (a hard attack
    softened); the sound keeps its level relative to the voice (the original's speech after
    <to> levelled like ours).

Piper (the old voices, kept to reproduce them):
  make_voice.py --engine piper --piper <venv>/bin/piper --model en_US-ljspeech-high.onnx [--only <name>...]
  make_voice.py --engine piper ... --model en_US-norman-medium.onnx --lines Sound/Voice/Ground/lines.txt --pitch 1
    Piper at a slightly brisk pace, pitched up a semitone (formants kept), the lows under
    120 Hz cut, a little presence (3 kHz), -18 dBFS RMS. (en_US-ljspeech-high: models MIT, LJ
    Speech public domain; en_US-norman: LibriVox, public domain.)

Needs ffmpeg (with rubberband for Piper) and numpy; Qwen3 also torch, qwen-tts,
faster-whisper, soundfile, torchaudio."""
import argparse, difflib, json, os, re, subprocess, sys, tempfile, time, wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
VOICE = os.path.join(ROOT, "Sound", "Voice")
VOICES = os.path.join(HERE, "voices")
RMS_DB, PEAK_DB = -18.0, -1.0
QWEN = "Qwen/Qwen3-TTS-12Hz-1.7B-Base"
MAX_SEEDS = 12                      # tries per take before a line is reported as failing


def read_lines(path, beds=None):
    """[(name, text, voice or None, takes)]; beds (a dict) gets name -> "<file>@<s>" """
    out = []
    for l in open(path, encoding="utf-8"):
        l = l.strip()
        if l and not l.startswith("#"):
            f = [s.strip() for s in l.split("|")]
            out.append((f[0], f[1], f[2] if len(f) > 2 and f[2] else None, int(f[3]) if len(f) > 3 and f[3] else 1))
            if beds is not None and len(f) > 4 and f[4]:
                beds[f[0]] = f[4]
    return out


def level(x):
    rms = np.sqrt(np.mean(x[np.abs(x) > 1e-3] ** 2))   # (the speech, not the pauses)
    return min(10 ** (RMS_DB / 20) / rms, 10 ** (PEAK_DB / 20) / np.abs(x).max())


# ---------------------------------------------------------------- Piper
def read_wav(path):
    with wave.open(path) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
        return x.reshape(-1, w.getnchannels()).mean(axis=1), w.getframerate()


def piper(a, lines, outdir):
    with tempfile.TemporaryDirectory() as tmp:
        for name, text, _, _ in lines:
            raw, shaped = os.path.join(tmp, "raw.wav"), os.path.join(tmp, "shaped.wav")
            subprocess.run([a.piper, "-m", a.model, "-f", raw, "--length-scale", "0.92", "--sentence-silence", "0"],
                           input=text.encode(), check=True, stderr=subprocess.DEVNULL)
            af = (f"rubberband=pitch={a.pitch:.5f}:formant=preserved,highpass=f=120,"
                  "equalizer=f=3000:t=q:w=1.0:g=3,aresample=48000,"
                  "silenceremove=start_periods=1:start_threshold=-50dB,adelay=30,apad=pad_dur=0.05")
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", raw, "-af", af, "-ac", "1", "-c:a", "pcm_s16le", shaped], check=True)
            x, sr = read_wav(shaped)
            g = level(x)
            out = os.path.join(outdir, name + ".flac")
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", shaped, "-af", f"volume={g:.6f}",
                            "-c:a", "flac", "-sample_fmt", "s16", out], check=True)
            print(f"{name}.flac: {len(x) / sr:.2f} s, gain {20 * np.log10(g):+.1f} dB  \"{text}\"")


# ---------------------------------------------------------------- Whisper's gate
ONES = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen " \
       "seventeen eighteen nineteen".split()
TENS = "_ _ twenty thirty forty fifty sixty seventy eighty ninety".split()


def num_words(n):
    if n < 20:
        return ONES[n]
    if n < 100:
        return TENS[n // 10] + (" " + ONES[n % 10] if n % 10 else "")
    if n < 1000:
        return ONES[n // 100] + " hundred" + (" " + num_words(n % 100) if n % 100 else "")
    return num_words(n // 1000) + " thousand" + (" " + num_words(n % 1000) if n % 1000 else "")


SOUNDS_LIKE = {"locks": "lox", "lock": "lox"}     # Whisper's spelling of what was said


def norm_words(text):
    """Words as said, comparable between the script and what Whisper heard: lower case,
    digits as words ("1,500" "0.8" "XR1" "O2"), "%" as percent, hyphens split, and runs of
    single letters joined ("A P U" and "APU" both "apu")."""
    t = text.lower().replace("%", " percent ")
    t = re.sub(r"(?<=\d),(?=\d{3})", "", t)
    t = re.sub(r"(\d+)\.(\d+)", lambda m: f"{m.group(1)} point {' '.join(m.group(2))}", t)
    t = re.sub(r"(?<=[a-z])(?=\d)|(?<=\d)(?=[a-z])", " ", t)
    t = re.sub(r"\d+", lambda m: " " + num_words(int(m.group())) + " ", t)
    toks = [SOUNDS_LIKE.get(w, w) for w in re.findall(r"[a-z]+", t)]
    out, i = [], 0
    while i < len(toks):                                # runs of 2+ single letters -> one word
        j = i
        while j < len(toks) and len(toks[j]) == 1:
            j += 1
        if j - i >= 2:
            out.append("".join(toks[i:j]))
            i = j
        else:
            out.append(toks[i])
            i += 1
    return out


END_STOP_DB = -32.0    # the speech's last 20 ms louder than this (re its loudest 10 ms): stopped mid-word
END_TAIL_DB = -40.0    # the take's last 20 ms louder than this: it never reaches silence (a hum, a breath)


def ending(x, sr):
    """(the level of the speech's last 20 ms, of the take's last 20 ms), dB re its loudest 10 ms"""
    env, _ = envelope(x.astype(np.float64), sr, 0.01)
    db = 20 * np.log10(env / max(env.max(), 1e-12) + 1e-12)
    active = np.nonzero(db > -45.0)[0]
    last = active[-1] if len(active) else len(db) - 1
    return float(db[max(0, last - 1): last + 1].max()), float(db[-2:].max())


def f0_track(x, sr, hop_s=0.01, win_s=0.04, fmin=70.0, fmax=400.0):
    """the pitch [Hz] each 10 ms (nan: unvoiced), by normalized autocorrelation"""
    hop, win, lo, hi = int(hop_s * sr), int(win_s * sr), int(sr / fmax), int(sr / fmin)
    starts = list(range(0, len(x) - win - hi, hop))
    rms = np.array([np.sqrt(np.mean(x[i:i + win] ** 2)) for i in starts])
    f0 = np.full(len(starts), np.nan)
    if not len(rms):
        return f0
    lags = np.arange(lo, hi)
    for k, i in enumerate(starts):
        if rms[k] < rms.max() * 10 ** (-30 / 20):
            continue
        a = x[i:i + win + hi] - np.mean(x[i:i + win + hi])
        seg = a[:win]
        idx = lags[:, None] + np.arange(win)[None, :]
        B = a[idx]
        c = (B @ seg) / np.sqrt(np.dot(seg, seg) * np.einsum("ij,ij->i", B, B) + 1e-12)
        j = int(np.argmax(c))
        if c[j] > 0.6:
            f0[k] = sr / lags[j]
    return f0


def end_rise(x, sr, tail_s=0.15, before_s=0.40):
    """[semitones] the pitch of the last voiced 150 ms against the 400 ms of voice before it:
    > 0 an upward, question-like ending (nan: too little voice to tell)"""
    f0 = f0_track(x.astype(np.float64), sr)
    v = np.nonzero(~np.isnan(f0))[0]
    if len(v) < 20:
        return float("nan")
    nt, nb = int(tail_s / 0.01), int(before_s / 0.01)
    med = np.median(f0[v])
    fold = lambda a: a * 2.0 ** np.round(np.log2(med / a))      # (octave errors folded)
    return float(12 * np.log2(np.median(fold(f0[v[-nt:]])) / np.median(fold(f0[v[-(nt + nb):-nt]]))))


def gate(text, heard, dur, peak, end=None, words=True, rise=None, max_rise=None, end_stop=END_STOP_DB, sim=None, min_sim=None):
    """None if the take is acceptable, else why not. end: ending() of the take; words: check
    the words too (off: a spelling test, "ay pee you", that Whisper writes "APU")."""
    a, b = norm_words(text), norm_words(heard)
    if not b:
        return "nothing heard"
    if not words:
        b = a
    # off script: compared letter by letter, so word splits don't count ("Cross-feed" / "Crossfeed")
    if difflib.SequenceMatcher(a="".join(a), b="".join(b), autojunk=False).ratio() < 0.6:
        return f"off script ({heard!r})"
    if not any(difflib.SequenceMatcher(a=a[-1], b=w).ratio() >= 0.6 for w in b[-3:]):   # ("dump" ~ "stump")
        return f"cut off ({heard!r})"
    if len("".join(b)) > len("".join(a)) * 1.5 + 6:
        return f"extra words ({heard!r})"
    n = len(a)
    if dur > 1.5 + 0.75 * n or dur < 0.12 * n:
        return f"{dur:.1f} s for {n} words"
    if peak >= 0.999:
        return "clipped"
    if end and end[0] > end_stop:
        return f"stops mid-word (its last 20 ms at {end[0]:.0f} dB)"
    if end and end[1] > END_TAIL_DB:
        return f"no clean ending (still {end[1]:.0f} dB at its end)"
    if max_rise is not None and rise is not None and rise > max_rise:
        return f"ends rising ({rise:+.1f} semitones)"
    if min_sim is not None and sim is not None and sim < min_sim:
        return f"a different voice (similarity {sim:.3f})"
    return None


# ---------------------------------------------------------------- shaping
def envelope(x, sr, frame_s=0.01):
    f = max(1, int(frame_s * sr))
    n = len(x) // f
    return np.sqrt((x[: n * f].reshape(n, f) ** 2).mean(axis=1)), f


def compress_pauses(x, sr, max_gap_s=0.70, keep_s=0.55, below_peak_db=40.0, xfade_s=0.012):
    """Shorten every pause inside the speech longer than max_gap_s to keep_s."""
    env, f = envelope(x, sr)
    quiet = env < env.max() * 10 ** (-below_peak_db / 20)
    loud = np.nonzero(~quiet)[0]
    if len(loud) < 2:
        return x
    cuts, i = [], loud[0]
    while i <= loud[-1]:
        if quiet[i]:
            j = i
            while j <= loud[-1] and quiet[j]:
                j += 1
            if (j - i) * f > max_gap_s * sr:
                half = int(keep_s * sr / 2)
                cuts.append((i * f + half, j * f - half))
            i = j
        else:
            i += 1
    if not cuts:
        return x
    xf, ramp, segs, pos = int(xfade_s * sr), np.linspace(0, 1, int(xfade_s * sr)), [], 0
    for a, b in cuts:
        segs.append(x[pos:a])
        pos = b
    segs.append(x[pos:])
    y = segs[0]
    for s in segs[1:]:
        y = np.concatenate([y[:-xf], y[-xf:] * (1 - ramp) + s[:xf] * ramp, s[xf:]]) if len(y) >= xf and len(s) >= xf else np.concatenate([y, s])
    return y


def trim_silence(x, sr, below_peak_db=45.0, pad_s=0.02):
    env, f = envelope(x, sr)
    on = np.nonzero(env > env.max() * 10 ** (-below_peak_db / 20))[0]
    if len(on) == 0:
        return x
    pad = int(pad_s * sr)
    return x[max(0, on[0] * f - pad): min(len(x), (on[-1] + 1) * f + pad)]


def to48k(path_or_x, sr=None, af="aresample=48000:resampler=soxr"):
    """mono float64 at 48 kHz, through ffmpeg (a file, or samples at sr)"""
    import soundfile as sf
    with tempfile.TemporaryDirectory() as tmp:
        a, b = os.path.join(tmp, "a.wav"), os.path.join(tmp, "b.wav")
        if not isinstance(path_or_x, str):
            sf.write(a, path_or_x.astype(np.float32), sr, subtype="FLOAT")
            path_or_x = a
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", path_or_x, "-af", af,
                        "-ac", "1", "-c:a", "pcm_f32le", b], check=True)
        return sf.read(b, dtype="float64")[0]


def shape(x, sr, out, bed=None):
    """pauses capped, trimmed, high-passed, 48 kHz, levelled, 30 ms lead -> FLAC; after a bed's
    first seconds, if given ("<sound file>@<s>", the file relative to the repository)"""
    import soundfile as sf
    x = trim_silence(compress_pauses(x.astype(np.float64), sr), sr)
    y = to48k(x, sr, "highpass=f=120,aresample=48000:resampler=soxr,adelay=30,apad=pad_dur=0.05")
    g = level(y)
    y = y * g
    if bed:
        f, spec = bed.rsplit("@", 1)
        span, _, soft = spec.partition(",")
        t0, _, t1 = span.rpartition("-")
        b = to48k(os.path.join(ROOT, f.replace("\\", "/")))
        n0, n1 = int(float(t0 or 0) * 48000), int(float(t1) * 48000)
        said = b[n1:]                                   # the original's line: levelled as ours is
        gb = 10 ** (RMS_DB / 20) / np.sqrt(np.mean(said[np.abs(said) > 1e-3] ** 2))
        head = b[n0:n1] * gb
        if soft:                                        # its attack softened: <dB> over <s>, back over 50 ms
            sdb, ss = (float(v) for v in soft.split("/"))
            env = np.ones(len(head))
            k, r = int(ss * 48000), int(0.05 * 48000)
            env[:k] = 10 ** (sdb / 20)
            env[k:k + r] = np.linspace(10 ** (sdb / 20), 1, len(env[k:k + r]))
            head *= env
        k = int(0.03 * 48000)
        head[-k:] *= np.linspace(1, 0, k)
        if n0:
            head[:k] *= np.linspace(0, 1, k)
        y = np.concatenate([head, y])
        y *= min(1.0, 10 ** (PEAK_DB / 20) / np.abs(y).max())
    sf.write(out, np.clip(y, -1, 1), 48000, subtype="PCM_16")
    return len(y) / 48000, g


# ---------------------------------------------------------------- Qwen3
def qwen3(a, lines, outdir, takes_path, beds):
    import soundfile as sf, torch, torchaudio.functional as AF
    from faster_whisper import WhisperModel
    from qwen_tts import Qwen3TTSModel
    kept = json.load(open(takes_path)) if os.path.exists(takes_path) else {}
    refs = {}
    for l in open(os.path.join(VOICES, "voices.txt"), encoding="utf-8"):
        if l.strip() and not l.startswith("#"):
            v, t = l.strip().split("|", 1)
            refs[v] = t
    t0 = time.time()
    model = Qwen3TTSModel.from_pretrained(QWEN, device_map="cuda:0", dtype=torch.bfloat16, attn_implementation="sdpa")
    whisper = WhisperModel("small.en", device="cpu", compute_type="int8")
    prompts = {}

    def prompt(v):
        if v not in prompts:
            x, sr = sf.read(os.path.join(VOICES, v + ".flac"), dtype="float32")
            prompts[v] = model.create_voice_clone_prompt(ref_audio=(x, sr), ref_text=refs[v], x_vector_only_mode=False)
        return prompts[v]

    def say(v, text, seed):
        torch.manual_seed(seed)
        w, sr = model.generate_voice_clone(text=text, language="English", voice_clone_prompt=prompt(v))
        return np.asarray(w[0], dtype=np.float32), sr

    refemb = {}

    def similarity(v, x, sr):
        """the take's speaker embedding (Qwen's x-vector) against its reference's: cosine"""
        if a.min_voice_sim is None:
            return None
        SR = model.model.speaker_encoder_sample_rate
        def emb(y, r):
            y = AF.resample(torch.from_numpy(np.ascontiguousarray(y, dtype=np.float32)), r, SR).numpy() if r != SR else y.astype(np.float32)
            e = model.model.extract_speaker_embedding(audio=y, sr=SR).float().flatten().cpu().numpy()
            return e / np.linalg.norm(e)
        if v not in refemb:
            rx, rsr = sf.read(os.path.join(VOICES, v + ".flac"), dtype="float32")
            refemb[v] = emb(rx, rsr)
        return float(np.dot(refemb[v], emb(x, sr)))

    def hear(x, sr):
        segs, _ = whisper.transcribe(AF.resample(torch.from_numpy(x), sr, 16000).numpy(), beam_size=5, language="en")
        return " ".join(s.text.strip() for s in segs)

    print(f"loaded in {time.time() - t0:.0f} s", flush=True)
    if a.keep:
        return keep(a, lines, outdir, kept, takes_path, beds, say)
    if a.candidates:
        return candidates(a, lines, kept, takes_path, beds, say, hear, similarity)
    failed = []
    for name, text, v, ntakes in lines:
        v = v or a.voice
        rec = kept.get(name)
        if rec and (rec["text"] != text or rec["voice"] != v):
            rec = None                                          # the line changed: start over
        if rec and a.reroll:
            rec = {**rec, "takes": [], "tried": rec.get("tried", [])}
        if rec is None:
            rec = {"text": text, "voice": v, "takes": [], "tried": []}
        files = [f"{name}.flac"] if ntakes == 1 else [f"{name}_{k + 1}.flac" for k in range(ntakes)]
        for k, fn in enumerate(files):
            out = os.path.join(outdir, fn)
            have = rec["takes"][k] if k < len(rec["takes"]) else None
            if have and os.path.exists(out) and not a.force:
                continue
            t0 = time.time()
            if have:                                            # rebuild from the kept seed
                x, sr = say(v, text, have["seed"])
                dur, g = shape(x, sr, out, beds.get(name))
                print(f"{fn}: seed {have['seed']} (kept), {dur:.2f} s", flush=True)
                continue
            seeds = (s for s in range(1, 10_000) if s not in rec["tried"])
            for _ in range(MAX_SEEDS):
                s = next(seeds)
                rec["tried"].append(s)
                x, sr = say(v, text, s)
                heard = hear(x, sr)
                why = gate(text, heard, len(x) / sr, float(np.abs(x).max()), ending(x, sr),
                           rise=end_rise(x, sr) if a.max_rise is not None else None, max_rise=a.max_rise, end_stop=a.end_stop,
                           sim=similarity(v, x, sr), min_sim=a.min_voice_sim)
                if why is None:
                    dur, g = shape(x, sr, out, beds.get(name))
                    rec["takes"][k:k + 1] = [{"seed": s, "heard": heard}]
                    print(f"{fn}: seed {s}, {dur:.2f} s, {time.time() - t0:.1f} s  \"{heard}\"", flush=True)
                    break
                print(f"  {fn}: seed {s} rejected: {why}", flush=True)
            else:
                failed.append(fn)
                print(f"{fn}: FAILED after {MAX_SEEDS} seeds", flush=True)
            kept[name] = rec
            json.dump(kept, open(takes_path, "w"), indent=1, sort_keys=True)
        kept[name] = rec
    json.dump(kept, open(takes_path, "w"), indent=1, sort_keys=True)
    if failed:
        print(f"{len(failed)} failed: {', '.join(failed)}")
    return 1 if failed else 0


def candidates(a, lines, kept, takes_path, beds, say, hear, similarity):
    """N new accepted takes of each line into a.review, for a person to choose from"""
    os.makedirs(a.review, exist_ok=True)
    cpath = os.path.join(a.review, "candidates.json")
    cand = json.load(open(cpath)) if os.path.exists(cpath) else {}
    for name, text, v, _ in lines:
        v = v or a.voice
        rec = kept.get(name) or {"text": text, "voice": v, "takes": [], "tried": []}
        if rec["text"] != text or rec["voice"] != v:
            rec = {"text": text, "voice": v, "takes": [], "tried": []}
        got, tries = [], 0
        seeds = (s for s in range(1, 10_000) if s not in rec["tried"])
        while len(got) < a.candidates and tries < MAX_SEEDS * a.candidates:
            s = next(seeds)
            tries += 1
            rec["tried"].append(s)
            x, sr = say(v, text, s)
            heard = hear(x, sr)
            why = gate(text, heard, len(x) / sr, float(np.abs(x).max()), ending(x, sr), not a.no_word_check,
                       rise=end_rise(x, sr) if a.max_rise is not None else None, max_rise=a.max_rise, end_stop=a.end_stop,
                       sim=similarity(v, x, sr), min_sim=a.min_voice_sim)
            if why:
                print(f"  {name}: seed {s} rejected: {why}", flush=True)
                continue
            dur, _ = shape(x, sr, os.path.join(a.review, f"{name}__s{s}.flac"), beds.get(name))
            got.append({"seed": s, "heard": heard})
            print(f"{name}: candidate seed {s}, {dur:.2f} s  \"{heard}\"", flush=True)
        cand[name] = {"text": text, "voice": v, "bed": beds.get(name), "takes": got,
                      "kept": rec["takes"][0]["seed"] if rec.get("takes") else None}
        kept[name] = rec
        json.dump(kept, open(takes_path, "w"), indent=1, sort_keys=True)
        json.dump(cand, open(cpath, "w"), indent=1, sort_keys=True)
    return 0


def keep(a, lines, outdir, kept, takes_path, beds, say):
    """the chosen seeds become the kept takes (and their files)"""
    want = dict(k.rsplit("=", 1) for k in a.keep)
    for name, text, v, ntakes in lines:
        if name not in want:
            continue
        v, s = v or a.voice, int(want[name])
        rec = kept.get(name) or {"text": text, "voice": v, "takes": [], "tried": []}
        if rec["text"] != text or rec["voice"] != v:
            rec = {"text": text, "voice": v, "takes": [], "tried": []}
        x, sr = say(v, text, s)
        dur, _ = shape(x, sr, os.path.join(outdir, name + ".flac"), beds.get(name))
        heard = ""
        if a.review and os.path.exists(os.path.join(a.review, "candidates.json")):
            c = json.load(open(os.path.join(a.review, "candidates.json"))).get(name, {})
            heard = next((t["heard"] for t in c.get("takes", []) if t["seed"] == s), "")
        rec["takes"] = [{"seed": s, "heard": heard}]
        if s not in rec["tried"]:
            rec["tried"].append(s)
        kept[name] = rec
        print(f"{name}.flac: seed {s} kept, {dur:.2f} s", flush=True)
    json.dump(kept, open(takes_path, "w"), indent=1, sort_keys=True)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--engine", choices=("qwen3", "piper"), default="qwen3")
    ap.add_argument("--lines", default=os.path.join(VOICE, "lines.txt"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--voice", default="crew", help="qwen3: the voice for lines that don't name one")
    ap.add_argument("--reroll", action="store_true", help="qwen3: drop the kept takes of --only lines, try new seeds")
    ap.add_argument("--force", action="store_true", help="qwen3: remake files that exist (from their kept seeds)")
    ap.add_argument("--candidates", type=int, help="qwen3: N new accepted takes of each --only line into --review")
    ap.add_argument("--review", help="qwen3: where --candidates go (and --keep reads what Whisper heard)")
    ap.add_argument("--max-rise", type=float, help="qwen3: reject takes whose pitch rises more than this "
                    "[semitones] at the end (a statement said like a question; Mission Control: 0.5)")
    ap.add_argument("--end-stop", type=float, default=END_STOP_DB, help="qwen3: the level [dB re the loudest] above "
                    "which a take's last 20 ms of voice count as stopping mid-word (a voice that ends firmly: higher)")
    ap.add_argument("--min-voice-sim", type=float, help="qwen3: reject a take whose voice drifts from its reference "
                    "(the cosine of Qwen's speaker embeddings below this; capcom1: 0.945)")
    ap.add_argument("--no-word-check", action="store_true", help="qwen3 --candidates: don't compare the words (a spelling test)")
    ap.add_argument("--keep", nargs="*", help='qwen3: "<name>=<seed>"... make these the kept takes')
    ap.add_argument("--piper")
    ap.add_argument("--model")
    ap.add_argument("--pitch", type=float, default=2 ** (1 / 12))
    a = ap.parse_args()
    if a.reroll and not a.only:
        ap.error("--reroll needs --only")
    if a.candidates and not (a.only and a.review):
        ap.error("--candidates needs --only and --review")
    if a.keep:
        a.only = [k.rsplit("=", 1)[0] for k in a.keep]
    outdir = os.path.dirname(os.path.abspath(a.lines))
    beds = {}
    lines = [l for l in read_lines(a.lines, beds) if not a.only or l[0] in a.only]
    if a.only and len(lines) < len(set(a.only)):
        ap.error("not in the lines file: " + ", ".join(set(a.only) - {l[0] for l in lines}))
    if a.engine == "piper":
        if not (a.piper and a.model):
            ap.error("--engine piper needs --piper and --model")
        return piper(a, lines, outdir)
    return qwen3(a, lines, outdir, os.path.splitext(a.lines)[0] + ".takes.json", beds)


if __name__ == "__main__":
    sys.exit(main())
