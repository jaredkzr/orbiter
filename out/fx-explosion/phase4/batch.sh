#!/bin/bash
# phase 4 deliverables on the final build (one session at a time): sheets (the standard times, and
# the flash's first frames), the close-up, the shimmer pair, the vacuum shell, videos (the wreck-view
# frame dropped as phase3/batch2.sh), the jolt observers (200 m: clip + plot; 1 km: plot), perf
# (1080p/1440p, the sea case and close), validation, ctest, A/B sheets against phase 3
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase4
for sc in sea land night; do $R $sc $P/$sc | tail -1; done
for sc in sea land night; do $R $sc $P/${sc}flash --times 0.033,0.067,0.1,0.15,0.2,0.3 --tag ${sc}flash | tail -1; done
$R close $P/close --size 1920x1080 --times 0.1,0.3,0.5,0.8,1.2,1.6,2.0,3.0 | tail -1
$R shimmer $P/shimmer --size 1920x1080 --times 0.5,0.8,1.2,2,3,4 | tail -1
ORBITER_VK_SHIMMER=0 $R shimmer $P/shimmeroff --size 1920x1080 --times 0.5,0.8,1.2,2,3,4 | tail -1
python3 $P/shimmer_pair.py $P/shimmeroff $P/shimmer $P/shimmer_pair.png
$R vacuum $P/vacuum --size 1920x1080 --times 0.033,0.067,0.1,0.15,0.2,0.3,0.4,0.5 | tail -1
$R vacspace $P/vacspace --size 1920x1080 --times 0.033,0.067,0.1,0.15,0.2,0.3,0.4,0.5 | tail -1
for sc in sea land night; do
  $R $sc $P/${sc}vid --video 1 10 --size 1920x1080 --times 12 | tail -1
  python3 - $P/${sc}vid/video <<'PY'
import os, sys
d = sys.argv[1]; fs = sorted(f for f in os.listdir(d) if f.startswith("f_"))
os.makedirs(d + "/enc", exist_ok=True)
k = 0
for i, f in enumerate(fs):
    if i == 30: continue  # (the frame of the wreck view the camera cuts to as the vessel is destroyed)
    os.symlink(os.path.abspath(os.path.join(d, f)), os.path.join(d, "enc", "e_%05d.png" % k)); k += 1
PY
  ffmpeg -y -loglevel error -framerate 30 -i $P/${sc}vid/video/enc/e_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/${sc}_crash.mp4 && echo "video $sc ok"
done
$R jolt200 $P/jolt200 --video 0.5 2.5 --size 1920x1080 --times 3 | tail -1
python3 - $P/jolt200/video <<'PY'
import os, sys
d = sys.argv[1]; fs = sorted(f for f in os.listdir(d) if f.startswith("f_"))
os.makedirs(d + "/enc", exist_ok=True)
k = 0
for i, f in enumerate(fs):
    if i == 15: continue
    os.symlink(os.path.abspath(os.path.join(d, f)), os.path.join(d, "enc", "e_%05d.png" % k)); k += 1
PY
ffmpeg -y -loglevel error -framerate 30 -i $P/jolt200/video/enc/e_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/jolt_200m.mp4 && echo "video jolt ok"
$R jolt1k $P/jolt1k --video 0.5 4 --times 3 | tail -1
python3 $P/jolt.py $P/jolt200 $P/jolt1k $P/jolt.png > $P/jolt.txt; head -3 $P/jolt.txt
for sz in 1920x1080:1080 2560x1440:1440; do
  s=${sz%%:*}; n=${sz##*:}
  $R sea $P/perf$n --ft --size $s | tail -1
  $R close $P/perfclose$n --ft --size $s --times 0.3,0.8,1.2,2,3 | tail -1
done
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R sea $P/validation | tail -1
echo "validation lines: $(grep -ci 'validation' $P/validation/orbiter.log)"; grep -i "validation" $P/validation/orbiter.log | head -5
(cd out/build/linux-x64-headless-release && ctest -R "Particles|Water|Damage|Debris" 2>&1 | tail -6)
for sc in sea land night; do python3 out/fx-explosion/phase3/ab.py $sc out/fx-explosion/phase3/$sc $P/$sc "phase 3" "phase 4" $P/ab_$sc.png; done
python3 out/fx-explosion/perf.py out/fx-explosion/phase3/perf1080 $P/perf1080 > $P/perf1080.txt
python3 out/fx-explosion/perf.py out/fx-explosion/phase3/perf1440 $P/perf1440 > $P/perf1440.txt
python3 out/fx-explosion/perf.py out/fx-explosion/phase3/perfclose1080 $P/perfclose1080 > $P/perfclose1080.txt
python3 out/fx-explosion/perf.py out/fx-explosion/phase3/perfclose1440 $P/perfclose1440 > $P/perfclose1440.txt
python3 out/fx-explosion/phase2/fbperf.py $P/perf1080 $P/perf1440 $P/perfclose1080 $P/perfclose1440 > $P/fbperf.txt
echo BATCH DONE
