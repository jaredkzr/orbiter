#!/bin/bash
# phase 6, second pass on the final build: the cut and the pull-away done by the script (the remote's lua
# has its own interpreter), warp 10x and lod8k after the WantsVolume distance fix, twinmerge after the
# flash-step merge, the volume's end (the pale flash fix), perf again, the bench Crash flight, validation, ctest
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase6
S=1920x1080
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
$R seacut $P/cut --size $S --cut 0.8 --times 0.5,1,2 | tail -1; grep -c "camera cut to the second" $P/cut/orbiter.log
$R sea $P/pullvid --size $S --pull 0.3 170 8000 4 --video 0.5 6 --times 6 | tail -1; enc $P/pullvid lod_pullaway 15
python3 $P/pullcheck.py $P/pullvid $P/pull_check.png | head -6
$R sea $P/pullfast --size $S --pull 0.3 170 8000 1.5 --video 0.5 4 --times 4 | tail -1; enc $P/pullfast lod_pullaway_fast 15
python3 $P/pullcheck.py $P/pullfast $P/pullfast_check.png | head -6
grep "stage fireball" $P/pullfast/orbiter.log | tail -4
$R sea $P/warp10 --size $S --warp 10 --times 0.3,0.7,1,1.3,2,3,5,8 | tail -1; grep -c "stage fireball.*sprites" $P/warp10/orbiter.log
$R twinmerge $P/twinmerge --size $S --times 0.1,0.3,0.5,1,1.5,2,3,5 | tail -1; grep "overlap\|fireball model" $P/twinmerge/orbiter.log | head -4
$R lod8k $P/lod8k --size $S --times 0.5,1,2,3 | tail -1; grep "stage fireball at +0.00" $P/lod8k/orbiter.log
$R lod3k $P/lod3k --size $S --times 0.5,1,2,3 | tail -1
$R sea $P/seaend --size $S --video 0 4.3 --times 4.3 | tail -1
$R sea $P/perf1440 --ft --size 2560x1440 | tail -1
$R close $P/perfclose1440 --ft --size 2560x1440 --times 0.5,1,2,3 | tail -1
python3 out/fx-explosion/perf.py out/fx-explosion/phase5/perf1440 $P/perf1440 > $P/perf1440.txt
python3 out/fx-explosion/phase2/fbperf.py out/fx-explosion/phase5/perf1440 $P/perf1440 out/fx-explosion/phase4c/perfclose1440 $P/perfclose1440 $P/perftwin1440 $P/perflarge1440 > $P/fbperf.txt
python3 out/fx-explosion/perf.py $P/perfclose1440 $P/perftwin1440 $P/perflarge1440 > $P/perfothers1440.txt
rm -rf $P/bench; mkdir -p $P/bench && Tests/Bench/run.sh -o $P/bench Crash && python3 Tests/Bench/budget.py $P/bench | tee $P/bench/budget.txt
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R sea $P/validation --times 0.5,3,8 | tail -1
echo "validation lines (sea): $(grep -ci 'validation' $P/validation/orbiter.log)"
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R twin $P/validationtwin --times 0.5,3 | tail -1
echo "validation lines (twin): $(grep -ci 'validation' $P/validationtwin/orbiter.log)"
(cd out/build/linux-x64-headless-release && ctest -R "Particles|Water|Damage|Debris" 2>&1 | tail -8) | tee $P/ctest.txt
echo BATCH2 DONE
