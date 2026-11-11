#!/bin/bash
# phase 3 deliverables: videos, the peak close-up, validation, ctest (one session at a time)
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase3
for sc in sea land night; do
  $R $sc $P/${sc}vid --video 1 10 --size 1920x1080 --times 12 | tail -1
  ffmpeg -y -loglevel error -framerate 30 -i $P/${sc}vid/video/f_%05d.png -c:v libx264 -crf 20 -pix_fmt yuv420p $P/${sc}_crash.mp4 && echo "video $sc ok"
done
$R close $P/peak --size 1920x1080 --times 0.05,0.1,0.15,0.2,0.3,0.4,0.5,0.65 | tail -1
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R sea $P/validation | tail -1
grep -ci "validation" $P/validation/orbiter.log; grep -i "validation" $P/validation/orbiter.log | grep -vi "layer enabled\|ORBITER_VK_VALIDATION" | head -5
cd out/build/linux-x64-headless-release && ctest -R "Particles|Water|Damage|Debris" 2>&1 | tail -8
echo BATCH DONE
