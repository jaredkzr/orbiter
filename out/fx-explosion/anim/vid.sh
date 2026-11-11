#!/bin/bash
# videos of the smoothed motion (anim/): land, sea, the wide land view, Atlantis land
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/anim
for sc in land sea landwide large100l; do
  rm -rf $P/${sc}vid
  $R $sc $P/${sc}vid --video 1 14 --size 1920x1080 --times 15 | tail -1
  python3 - $P/${sc}vid/video <<'PY'
import os, sys
d = sys.argv[1]; fs = sorted(f for f in os.listdir(d) if f.startswith("f_"))
os.makedirs(d + "/enc", exist_ok=True)
k = 0
for i, f in enumerate(fs):
    if i == 30: continue
    os.symlink(os.path.abspath(os.path.join(d, f)), os.path.join(d, "enc", "e_%05d.png" % k)); k += 1
PY
  ffmpeg -y -loglevel error -framerate 30 -i $P/${sc}vid/video/enc/e_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/${sc}_crash.mp4 && echo "video $sc ok"
done
