#!/usr/bin/env python3
"""Coverage of Orbiter's voice: every XRSound default sound that Orbiter's code or the
shipped XRSound configs name, and every group line the callouts build (altitudes, Mach
numbers, docking distances), must either have Sound/Voice/<name>.flac or be one of
XRSound's non-speech sounds (not in Sound/Voice/lines.txt and no voice in it).

  check_voice.py      exit 1 and a list if a spoken line is missing

Run after make_voice.py; the in-game override is System::FindSound (AudioSystem.cpp)."""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
VOICE = os.path.join(ROOT, "Sound", "Voice")
XR = os.path.join(ROOT, "Sound", "XRSound", "assets", "XRSound")
CALLOUTS = os.path.join(ROOT, "Src", "Orbiter", "Audio", "Callouts.cpp")

ref = re.compile(r"XRSound[/\\]+Default[/\\]+([^/\\\"\n]+?)\.(?:wav|ogg|flac|mp3)\b", re.I)
named = {}                                        # name -> where it's named
srcs = [os.path.join(ROOT, "Src", "Orbiter", "Audio", f) for f in os.listdir(os.path.join(ROOT, "Src", "Orbiter", "Audio")) if f.endswith(".cpp")]
srcs += [os.path.join(XR, f) for f in os.listdir(XR) if f.endswith(".cfg")]
for p in srcs:
    for l in open(p, encoding="utf-8", errors="replace"):
        if l.lstrip().startswith(("#", "//", ";")) and not p.endswith(".cpp"):
            continue
        for m in ref.finditer(l):
            named.setdefault(m.group(1), os.path.relpath(p, ROOT))

src = open(CALLOUTS, encoding="utf-8").read()
heights = [int(h) for h in re.search(r"const int heights\[\] = \{([^}]*)\}", src).group(1).replace("\n", " ").split(",") if h.strip()]
groups = {str(h): "Callouts: altitude/distance group" for h in heights}
groups.update({f"Mach {m}": "Callouts: Mach group" for m in range(1, 27)})
groups["Mach 27 Plus"] = "Callouts: Mach group"
for k, v in groups.items():
    named.setdefault(k, v)

spoken = {l.split("|", 1)[0].strip().lower() for l in open(os.path.join(VOICE, "lines.txt"), encoding="utf-8") if "|" in l and not l.startswith("#")}
have = {f[:-5].lower() for f in os.listdir(VOICE) if f.endswith(".flac")}
xrfiles = {f.rsplit(".", 1)[0].lower() for f in os.listdir(os.path.join(XR, "Default"))}
# (names compared in any case, as Orbiter finds files)
missing = sorted(n for n in named if n.lower() in spoken and n.lower() not in have)
unlisted = sorted(n for n in named if n.lower() not in spoken and n.lower() not in xrfiles)
nonspeech = sorted(n for n in named if n.lower() not in spoken)
print(f"{len(named)} XRSound default sounds named ({len(groups)} group lines); "
      f"{len(named) - len(nonspeech)} spoken, {len(nonspeech)} non-speech (XRSound's sound plays: {', '.join(nonspeech)})")
if unlisted:
    print("named but no such XRSound file:", ", ".join(unlisted))
for n in sorted(groups):
    if n.lower() not in spoken:
        print(f"group line not in lines.txt: {n}")
if missing:
    print(f"{len(missing)} spoken line(s) without Sound/Voice/<name>.flac:")
    for n in missing:
        print(f"  {n}  ({named[n]})")
    sys.exit(1)
print("every spoken line has its Sound/Voice file")
