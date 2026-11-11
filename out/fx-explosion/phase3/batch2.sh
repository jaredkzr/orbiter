#!/bin/bash
# phase 3 deliverables on the final build (one session at a time): stills, videos (the camera-cut
# frame after the break-up dropped), the peak close-up, validation, ctest, the sheets
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase3
for sc in sea sunside land night; do $R $sc $P/$sc | tail -1; done
$R close $P/close --size 1920x1080 --times 0.1,0.3,0.5,0.8,1.2,1.6,2.0,3.0 | tail -1
$R close $P/peak --size 1920x1080 --times 0.05,0.1,0.15,0.2,0.3,0.4,0.5,0.65 | tail -1
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
for sz in 1920x1080:1080 2560x1440:1440; do
  s=${sz%%:*}; n=${sz##*:}
  $R sea $P/perf$n --ft --size $s | tail -1
  $R close $P/perfclose$n --ft --size $s --times 0.3,0.8,1.2,2,3 | tail -1
done
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R sea $P/validation | tail -1
echo "validation lines: $(grep -ci 'validation' $P/validation/orbiter.log)"; grep -i "validation" $P/validation/orbiter.log | head -5
(cd out/build/linux-x64-headless-release && ctest -R "Particles|Water|Damage|Debris" 2>&1 | tail -6)
for sc in sea land night; do python3 $P/ab.py $sc; done; python3 $P/sunshade.py
echo BATCH2 DONE
