#!/bin/bash
# VACUUM_PLAN V1: the air factors (drag, burn, confine) and no drag in vacuum
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/vac/v1
mkdir -p $P
$R sea $P/sea | tail -1
for sc in moon mars; do $R $sc $P/$sc | tail -1; done
for sc in alt30 alt50 alt60 alt70; do $R $sc $P/$sc --times 0.05,0.3,1,2,5,10 | tail -1; done
for d in $P/*/; do python3 out/fx-explosion/vac/classes.py $d 1,2,5,10,20; done > $P/classes.txt
for d in $P/*/; do echo "$(basename $d): $(grep -m1 'fireball .* fuel' $d/orbiter.log | sed 's/.*fuel/fuel/')"; done > $P/air.txt
echo V1 DONE
