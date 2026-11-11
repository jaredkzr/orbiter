#!/bin/bash
# phase 5 "before" sheets on the phase 4c build (one session at a time): the late sea and land
# times, the midair break-ups at 150 and 300 m/s
cd /home/jared/projects/orbiter
R="python3 out/fx-explosion/run.py"
P=out/fx-explosion/phase5
$R sea $P/before_sea --size 1920x1080 --times 2,3,4,5,6,8,10,12 --tag before_sea | tail -1
$R land $P/before_land --size 1920x1080 --times 3,5,8,10,12,15,20,25 --tag before_land | tail -1
$R midair $P/before_midair --size 1920x1080 --tag before_midair | tail -1
$R midair300 $P/before_midair300 --size 1920x1080 --tag before_midair300 | tail -1
echo PRE DONE
