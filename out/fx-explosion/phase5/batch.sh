#!/bin/bash
# phase 5 deliverables on the final build (one session at a time): the late sea and land sheets,
# the midair break-ups at 150 and 300 m/s, A/B against the phase 4c build (pre.sh), videos (the
# wreck-view frame dropped as phase4b/vid.sh), perf at 1440p against phase 4c, validation, ctest
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase5
AB=out/fx-explosion/phase3/ab.py
$R sea $P/sea --size 1920x1080 --times 2,3,4,5,6,8,10,12 --tag sea | tail -1
$R land $P/land --size 1920x1080 --times 3,5,8,10,12,15,20,25 --tag land | tail -1
$R midair $P/midair --size 1920x1080 --tag midair | tail -1
$R midair300 $P/midair300 --size 1920x1080 --tag midair300 | tail -1
for sc in sea land midair midair300; do python3 $AB $sc $P/before_$sc $P/$sc "phase 4c" "phase 5" $P/ab_$sc.png; done
for sc in sea:12 land:14 midair:8; do
  n=${sc%%:*}; post=${sc##*:}
  rm -rf $P/${n}vid
  $R $n $P/${n}vid --video 1 $post --size 1920x1080 --times $post | tail -1
  python3 - $P/${n}vid/video $n <<'PY'
import os, sys
d = sys.argv[1]; fs = sorted(f for f in os.listdir(d) if f.startswith("f_"))
os.makedirs(d + "/enc", exist_ok=True)
k = 0
for i, f in enumerate(fs):
    if i == 30 and sys.argv[2] != "midair": continue  # (the frame of the wreck view the camera cuts to as the vessel is destroyed)
    os.symlink(os.path.abspath(os.path.join(d, f)), os.path.join(d, "enc", "e_%05d.png" % k)); k += 1
PY
  ffmpeg -y -loglevel error -framerate 30 -i $P/${n}vid/video/enc/e_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/${n}_crash.mp4 && echo "video $n ok"
done
$R sea $P/perf1440 --ft --size 2560x1440 | tail -1
python3 out/fx-explosion/perf.py out/fx-explosion/phase4c/perf1440 $P/perf1440 > $P/perf1440.txt
python3 out/fx-explosion/phase2/fbperf.py out/fx-explosion/phase4c/perf1440 $P/perf1440 > $P/fbperf.txt
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R sea $P/validation --times 3,8 | tail -1
echo "validation lines: $(grep -ci 'validation' $P/validation/orbiter.log)"; grep -i "validation" $P/validation/orbiter.log | head -5
(cd out/build/linux-x64-headless-release && ctest -R "Particles|Water|Damage|Debris" 2>&1 | tail -8)
echo BATCH DONE
