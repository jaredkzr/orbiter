#!/bin/bash
# VACUUM_PLAN V0: baselines of the out-of-air scenes on the current build
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/vac/v0
mkdir -p $P
VT=0.033,0.1,0.2,0.3,0.5,1,2,5,10,20
for sc in vacuum vacspace; do $R $sc $P/$sc --times $VT | tail -1; done
for sc in moon mars; do $R $sc $P/$sc | tail -1; done
for sc in alt30 alt50 alt60 alt70 alt90; do $R $sc $P/$sc --times 0.05,0.3,1,2,5,10 | tail -1; done
for d in $P/*/; do python3 out/fx-explosion/vac/classes.py $d 1,2,5,10,20; done > $P/classes.txt
echo V0 DONE
