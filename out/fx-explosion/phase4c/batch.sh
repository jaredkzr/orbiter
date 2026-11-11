#!/bin/bash
# phase 4c deliverables on the final build (one session at a time): the measure runs with the
# camera shake off (strips, silhouette), the videos with it on (the wreck-view frame dropped as
# phase4b/vid.sh), the standard sheets (A/B against phase 4b), perf at 1440p (against phase4b/perf1440)
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase4c
B=out/fx-explosion/phase4b
# the measure runs: 1080p video frames, -0.3 s to 3 s, shake off (as phase 4b's seavid/landvid/seafastvid)
for sc in sea land seafast; do
  ORBITER_VK_SHAKE=0 $R $sc $P/${sc}vid --video 0.3 3 --size 1920x1080 --times 3 | tail -1
  python3 $B/strip.py $P/${sc}vid $P/strip_after_$sc.png
  cp $B/strip_after_$sc.png $P/strip_before_$sc.png
done
python3 $B/silhouette.py $P/silhouette.png "4b sea=$B/seavid" "4c sea=$P/seavid" "4b land=$B/landvid" "4c land=$P/landvid" "4b sea 60 m/s=$B/seafastvid" "4c sea 60 m/s=$P/seafastvid" > $P/silhouette.txt
cat $P/silhouette.txt
# the videos (shake on), 1 s before to 10 s after
for sc in sea land; do
  rm -rf $P/${sc}vid2
  $R $sc $P/${sc}vid2 --video 1 10 --size 1920x1080 --times 12 | tail -1
  python3 - $P/${sc}vid2/video <<'PY'
import os, sys
d = sys.argv[1]; fs = sorted(f for f in os.listdir(d) if f.startswith("f_"))
os.makedirs(d + "/enc", exist_ok=True)
k = 0
for i, f in enumerate(fs):
    if i == 30: continue  # (the frame of the wreck view the camera cuts to as the vessel is destroyed)
    os.symlink(os.path.abspath(os.path.join(d, f)), os.path.join(d, "enc", "e_%05d.png" % k)); k += 1
PY
  ffmpeg -y -loglevel error -framerate 30 -i $P/${sc}vid2/video/enc/e_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/${sc}_crash.mp4 && echo "video $sc ok"
done
# full-frame stills at the review's times (sea 1.6 s, land 1.0 s: video frame = 30 + t * 30)
python3 - <<'PY'
import os, shutil
P = "out/fx-explosion/phase4c"
for sc, t, name in (("sea", 1.6, "full_2p6.png"), ("land", 1.0, "land_2p0.png"), ("sea", 2.4, "sea_3p4.png"), ("land", 2.0, "land_3p0.png")):
    fs = sorted(f for f in os.listdir(f"{P}/{sc}vid2/video") if f.startswith("f_"))
    shutil.copy(f"{P}/{sc}vid2/video/{fs[30 + round(t * 30)]}", f"{P}/{name}")
PY
# the standard sheets and the A/B against phase 4b
for sc in sea land; do $R $sc $P/$sc | tail -1; python3 out/fx-explosion/phase3/ab.py $sc $B/$sc $P/$sc "phase 4b" "phase 4c" $P/ab_$sc.png; done
# perf (sea, 1440p) against phase 4b
$R sea $P/perf1440 --ft --size 2560x1440 | tail -1
python3 out/fx-explosion/perf.py $B/perf1440 $P/perf1440 > $P/perf1440.txt
python3 out/fx-explosion/phase2/fbperf.py $B/perf1440 $P/perf1440 > $P/fbperf.txt
echo BATCH DONE
