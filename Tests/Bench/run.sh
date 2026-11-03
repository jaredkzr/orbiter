#!/bin/bash
# Frame-rate benchmark flights (Vulkan client, offscreen).
#
#   Tests/Bench/run.sh [-b build_dir] [-o out_dir] [-r WxH] [flight ...]
#
# Flights: Atmos (DG, Mojave to Edwards, 1x), Ascent (Shuttle to ISS
# playback), Moon (DG to the Moon playback), Mars, Jupiter (1000 g
# relativistic autopilot to orbit), Proxima, Barnard (1 g, ship-time warp
# 10^6, to orbit), Nyx (1 g, to rest by the black hole, where holding takes
# a quarter of the drive). Default: all.
# Playbacks run on a compressed time-warp schedule (Script/Bench/play.lua),
# trips on Script/Bench/trip.lua.
#
# Per flight in out_dir: <name>.ft (ORBITER_VK_FRAMETIMES: frame, sim time,
# wall ms, GPU ms, GPU marks) and <name>.log; summary with analyze.py:
#   python3 Tests/Bench/analyze.py out_dir/*.ft
# Orbiter.cfg is backed up and restored; the window size and
# ReplayTimewarp = FALSE (playbacks at the script's warp) set for the runs.
set -u
SRC=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$SRC/out/build/linux-x64-headless-release
OUT=$PWD/bench-out
RES=2560x1440
while getopts "b:o:r:" o; do case $o in
	b) BUILD=$(cd "$OPTARG" && pwd) ;; o) OUT=$OPTARG ;; r) RES=$OPTARG ;; *) exit 2 ;;
esac; done
shift $((OPTIND - 1))
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
W=${RES%x*} H=${RES#*x}
cd "$BUILD" || exit 1
cp Orbiter.cfg "$OUT/Orbiter.cfg.orig"
trap 'cp "$OUT/Orbiter.cfg.orig" Orbiter.cfg' EXIT
grep -v "^WindowWidth\|^WindowHeight\|^ReplayTimewarp" "$OUT/Orbiter.cfg.orig" > "$OUT/Orbiter.cfg.bench"
printf 'ReplayTimewarp = FALSE\nWindowWidth = %s\nWindowHeight = %s\n' "$W" "$H" >> "$OUT/Orbiter.cfg.bench"
ulimit -v 12000000

run() { local name=$1 tmo=$2; shift 2
	cp "$OUT/Orbiter.cfg.bench" Orbiter.cfg
	local t0=$(date +%s)
	env "$@" ORBITER_VK_OFFSCREEN=1 ORBITER_VK_PROFILE=1 ORBITER_VK_FRAMETIMES="$OUT/$name.ft" \
		timeout "$tmo" ./Orbiter -p VulkanClient "--scenariox=$SRC/Tests/Bench/$name.scn" >/dev/null 2>&1
	local rc=$?
	cp Orbiter.log "$OUT/$name.log"
	echo "$name rc=$rc wall=$(( $(date +%s) - t0 ))s frames=$(wc -l < "$OUT/$name.ft") $(grep -a 'bench' Orbiter.log | tail -1 | cut -c13-)"
}
for n in ${@:-Atmos Ascent Moon Mars Jupiter Proxima Barnard Nyx}; do case $n in
	Atmos)   run Atmos 1500 BENCH_SCHED="0:1" BENCH_END=335 ;;
	Ascent)  run Ascent 2700 BENCH_SCHED="0:1,240:10,415:1,470:10,580:1,649:10,1102:100,1800:20,3110:10,3443:50,5290:100,29000:10" BENCH_END=29395 ;;
	Moon)    run Moon 1500 BENCH_SCHED="0:20,165:500,14415:20,15405:30,15965:20,16165:30,16870:20,17235:100,19250:20,19545:5000,281090:20,283650:500,304155:20,304580:50,307380:20" BENCH_END=308835 ;;
	Mars)    run Mars 1500 TRIP_TARGET=Mars TRIP_ARR=orbit TRIP_ALT=500e3 TRIP_G=1000 TRIP_WARP=100 ;;
	Jupiter) run Jupiter 1500 TRIP_TARGET=Jupiter TRIP_ARR=orbit TRIP_ALT=50e6 TRIP_G=1000 TRIP_WARP=100 ;;
	Proxima) run Proxima 1800 "TRIP_TARGET=Proxima b" TRIP_ARR=orbit TRIP_ALT=1000e3 TRIP_G=1 TRIP_SHIPWARP=1e6 ;;
	Barnard) run Barnard 1800 "TRIP_TARGET=Barnard b" TRIP_ARR=orbit TRIP_ALT=1000e3 TRIP_G=1 TRIP_SHIPWARP=1e6 ;;
	Nyx)     run Nyx 2700 TRIP_TARGET=Nyx TRIP_ARR=rest TRIP_ALT=default TRIP_G=1 TRIP_SHIPWARP=1e6 TRIP_WALLMAX=2400 ;;
	*) echo "unknown flight $n" ;;
esac; done
