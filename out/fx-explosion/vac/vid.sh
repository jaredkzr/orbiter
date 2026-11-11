#!/bin/bash
# vac/vid.sh PHASE SCENE... : 1080p videos, 0.5 s before the burst to 3 s after (the wreck-view frame dropped)
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/vac/$1; shift
mkdir -p $P
for sc in "$@"; do
  rm -rf $P/${sc}vid
  $R $sc $P/${sc}vid --video 0.5 3 --size 1920x1080 --times 3.2 | tail -1
  python3 - $P/${sc}vid/video <<'PY'
import os, sys
d = sys.argv[1]; fs = sorted(f for f in os.listdir(d) if f.startswith("f_"))
os.makedirs(d + "/enc", exist_ok=True)
k = 0
for i, f in enumerate(fs):
    if i == int(os.environ.get("DROP", "15")): continue  # (the wreck view the camera cuts to as the vessel is destroyed; DROP=-1: none)
    os.symlink(os.path.abspath(os.path.join(d, f)), os.path.join(d, "enc", "e_%05d.png" % k)); k += 1
PY
  ffmpeg -y -loglevel error -framerate 30 -i $P/${sc}vid/video/enc/e_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/${sc}.mp4 && echo "video $sc ok"
done
