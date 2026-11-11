#!/bin/bash
# phase 6, final build: perf at 1440p (sea, close) after the fading soot ball's coarser step, validation, ctest
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"; P=out/fx-explosion/phase6
set -x
$R sea $P/perf1440 --ft --size 2560x1440 | tail -1
$R close $P/perfclose1440 --ft --size 2560x1440 --times 0.5,1,2,3 | tail -1
python3 out/fx-explosion/perf.py out/fx-explosion/phase5/perf1440 $P/perf1440 > $P/perf1440.txt
python3 out/fx-explosion/phase2/fbperf.py out/fx-explosion/phase5/perf1440 $P/perf1440 out/fx-explosion/phase4c/perfclose1440 $P/perfclose1440 $P/perftwin1440 $P/perflarge1440 > $P/fbperf.txt
python3 out/fx-explosion/perf.py $P/perfclose1440 $P/perftwin1440 $P/perflarge1440 > $P/perfothers1440.txt
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R sea $P/validation --times 0.5,3,8 | tail -1
echo "validation lines (sea): $(grep -ci 'validation' $P/validation/orbiter.log)"
ORBITER_VK_VALIDATION=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT $R large100l $P/validation100 --times 0.5,3,8 | tail -1
echo "validation lines (atlantis): $(grep -ci 'validation' $P/validation100/orbiter.log)"
$R sea $P/sea --size 1920x1080 | tail -1
(cd out/build/linux-x64-headless-release && ctest -R "Particles|Water|Damage|Debris" 2>&1 | tail -8) | tee $P/ctest.txt
echo BATCH4 DONE
