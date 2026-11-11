#!/bin/bash
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"; P=out/fx-explosion/phase6; S=1920x1080
enc() {
  python3 - "$1/video" "$3" <<'PY'
import os, sys
d = sys.argv[1]; drop = int(sys.argv[2]) if sys.argv[2] else -1
fs = sorted(f for f in os.listdir(d) if f.startswith("f_"))
os.makedirs(d + "/enc", exist_ok=True)
for f in os.listdir(d + "/enc"): os.remove(os.path.join(d, "enc", f))
k = 0
for i, f in enumerate(fs):
    if i == drop: continue
    os.symlink(os.path.abspath(os.path.join(d, f)), os.path.join(d, "enc", "e_%05d.png" % k)); k += 1
PY
  ffmpeg -y -loglevel error -framerate 30 -i $1/video/enc/e_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/$2.mp4 && echo "video $2 ok"
}
set -x
$R sea $P/pullvid --size $S --pull 0.3 170 8000 4 --video 0.5 6 --times 6 | tail -1; enc $P/pullvid lod_pullaway 15
python3 $P/pullcheck.py $P/pullvid $P/pull_check.png | head -6; grep "stage fireball" $P/pullvid/orbiter.log | tail -3
$R sea $P/pullfast --size $S --pull 0.3 170 8000 1.5 --video 0.5 4 --times 4 | tail -1; enc $P/pullfast lod_pullaway_fast 15
python3 $P/pullcheck.py $P/pullfast $P/pullfast_check.png | head -6; grep "stage fireball" $P/pullfast/orbiter.log | tail -3
rm -rf $P/bench; mkdir -p $P/bench && Tests/Bench/run.sh -o $P/bench Crash && python3 Tests/Bench/budget.py $P/bench | tee $P/bench/budget.txt
echo BATCH3 DONE
