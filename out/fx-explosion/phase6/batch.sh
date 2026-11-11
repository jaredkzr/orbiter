#!/bin/bash
# phase 6 deliverables (one session at a time): the Atlantis 100 t (land, sea), the 300 kg DG, two DGs
# (twin: two fireballs; twinmerge: merged; midair: the collision, one ball), 10x warp, pause, cut,
# the LOD sweep (stills at 200 m .. 8 km, a pull-away video), the settings entry (sprites / volume /
# full), perf at 1440p against phase 5, the budget.py Crash flight, validation, ctest
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase6
S=1920x1080
enc() { # enc DIR NAME [DROP]: the video frames (the wreck-view frame DROP dropped) to NAME.mp4
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
$R large100l $P/large100l --size $S --times 0.1,0.5,1,2,3,4,6,10 | tail -1
$R large100l $P/large100lvid --size $S --video 1 10 --times 10 | tail -1; enc $P/large100lvid atlantis100_land_crash 30
$R large100 $P/large100 --size $S --times 0.1,0.5,1,2,3,4,6,10 | tail -1
$R small300 $P/small300 --size $S --times 0.05,0.1,0.3,0.5,0.8,1.2,2,3 | tail -1
$R twin $P/twin --size $S --times 0.1,0.3,0.5,1,1.5,2,3,5 | tail -1
$R twin $P/twinvid --size $S --video 1 7 --times 7 | tail -1; enc $P/twinvid twin_crash 30
$R twinmerge $P/twinmerge --size $S --times 0.1,0.3,0.5,1,1.5,2,3,5 | tail -1
$R midair $P/midair --size $S --times 0.1,0.3,0.5,1,1.5,2,3,5 | tail -1
$R midair $P/midairvid --size $S --video 1 7 --times 7 | tail -1; enc $P/midairvid midair_crash
$R sea $P/warp10 --size $S --warp 10 --times 0.3,0.7,1,1.3,2,3,5,8 | tail -1
$R sea $P/pause --size $S --pausecheck 0.8 --times 0.8,2 | tail -1
$R seacut $P/cut --size $S --cut 0.8 --times 0.5,1,2 | tail -1
for d in lod200 lod1k lod3k lod8k; do $R $d $P/$d --size $S --times 0.5,1,2,3 | tail -1; done
$R sea $P/pullvid --size $S --pull 0.3 170 8000 4 --video 0.5 6 --times 6 | tail -1; enc $P/pullvid lod_pullaway 15
ORBITER_VK_FIREBALL=sprites $R sea $P/set_sprites --size $S --times 0.5,1,2,3 | tail -1
ORBITER_VK_FIREBALL=full $R sea $P/set_full --size $S --times 0.5,1,2,3 | tail -1
printf 'Fireballs = Volumetric, full resolution\n' > $P/graphics_full.cfg
ORBITER_VK_GRAPHICS_CFG=$PWD/$P/graphics_full.cfg $R sea $P/set_cfgfull --size $S --times 0.5,1,2,3 | tail -1
grep -h "graphics settings\|Fireballs" $P/set_cfgfull/orbiter.log | head -3
$R sea $P/perf1440 --ft --size 2560x1440 | tail -1
$R close $P/perfclose1440 --ft --size 2560x1440 --times 0.5,1,2,3 | tail -1
$R twin $P/perftwin1440 --ft --size 2560x1440 --times 0.5,1,2,3 | tail -1
$R large100l $P/perflarge1440 --ft --size 2560x1440 --times 0.5,1,2,3,4 | tail -1
python3 out/fx-explosion/perf.py out/fx-explosion/phase5/perf1440 $P/perf1440 > $P/perf1440.txt
python3 out/fx-explosion/phase2/fbperf.py out/fx-explosion/phase5/perf1440 $P/perf1440 $P/perfclose1440 $P/perftwin1440 $P/perflarge1440 > $P/fbperf.txt
python3 out/fx-explosion/perf.py $P/perfclose1440 $P/perftwin1440 $P/perflarge1440 > $P/perfothers1440.txt
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R sea $P/validation --times 0.5,3,8 | tail -1
echo "validation lines (sea): $(grep -ci 'validation' $P/validation/orbiter.log)"
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R large100l $P/validation100 --times 0.5,3,8 | tail -1
echo "validation lines (atlantis): $(grep -ci 'validation' $P/validation100/orbiter.log)"
mkdir -p $P/bench && Tests/Bench/run.sh -o $P/bench Crash && python3 Tests/Bench/budget.py $P/bench | tee $P/bench/budget.txt
(cd out/build/linux-x64-headless-release && ctest -R "Particles|Water|Damage|Debris" 2>&1 | tail -8) | tee $P/ctest.txt
echo BATCH DONE
