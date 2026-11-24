#!/bin/bash
# Frame-rate benchmark flights (Vulkan client, offscreen).
#
#   Tests/Bench/run.sh [-b build_dir] [-o out_dir] [-r WxH] [-n N] [-F] [flight|suite ...]
#
# Flights: Atmos (DG, Mojave to Edwards, 1x), Ascent (Shuttle to ISS
# playback), Moon (DG to the Moon playback), Mars, Jupiter (1000 g
# relativistic autopilot to orbit), Proxima, Barnard (1 g, ship-time warp
# 10^6, to orbit), Nyx (1 g, to rest by the black hole, where holding takes
# a quarter of the drive). Default: all of these (not the others below).
# Launch (Shuttle launch from 39A, camera 850 m away, T+0-60 at 1x, particle
# density 4), Launch8 (the same at density 8), LaunchFar (seen from 42 km,
# T+0-240), LaunchNight (Launch at night: the flames light the smoke, its
# local-light occlusion), Crash (a DG with 7 t of propellant into the sea,
# observer 170 m aside: the crash explosion, 0-12 s after the break-up): the
# particle budget, checked by
#   python3 Tests/Bench/budget.py out_dir
# (GPU time of the particle passes, allocation failures, dropped emissions;
# ORBITER_VK_PARTICLE_STATS telemetry in the logs).
# DGHover (a Delta-glider lifting off on its hover engines at night, the
# Cape: the flame splash, its light, ground jets, dust shadows; then the climb
# away), Dock (Atlantis undocks from the ISS and fires its RCS jets in turn:
# cockpit, then the camera switched outside), NightCity (a camera flying
# 600 m over Houston at night at 200 m/s: cities, night lights, tile
# streaming): real-time steps.
# Pacing (hitches): CamCuts (a DG parked at Mojave: cockpit, a zoomed 4 deg
# ground observer, chase, a high observer, cut every 4 s, new places each
# round, the last round the first again: streaming after cuts, the tile cache,
# video memory at the zoom), CamToggle (cockpit and chase in turn every 1 s at
# one place: keeping the view just left), ColdStart (NightCity's first 10 s
# with an empty shader cache: the load, the time to the first and to steady
# frames).
# Heavy: HighWarp (a DG in low Earth orbit, the ground in view, time warp 1,
# 100, 1000, 10000: tile and ray tracing churn, the core's step), ManyVessels
# (50 vessels of six classes parked at the Cape, a ground observer flying in
# from 12 km, then panning along them: their visuals made as they come within
# range, texture uploads, the cost per vessel), Splashdown (a
# DG ditching at 70 m/s, the chase camera, then the wreck: spray, debris, the
# explosion, first use of the effects), Reentry (a DG at 70 km and 7 km/s,
# nose up 40 deg: the plasma; terrain streaming under a hypersonic descent),
# CloudDeck (a DG climbing on its main engines through the clouds over East
# Africa: the cloud passes, inside and out).
# Nightly: Soak (30 minutes in low orbit: a camera cut every 30 s, a time
# warp burst every 2 minutes; the heap and the process memory over time: no
# growth after the first minutes).
# Matrix (a run of its own): NightCity, DGHover and AtmosQ at 1080p, 1440p
# and 4K, with native TAA, FSR 3 and FSR 4, ray tracing on and off (fixed
# step, one run each: how the cost scales).
# Suites: quick (shortened Atmos, Launch, DGHover, Dock, NightCity, Crash:
# about 6 minutes, before committing performance work); pacing; heavy; all
# (every flight but Soak and the matrix); nightly (Soak); matrix.
# Playbacks run on a compressed time-warp schedule (Script/Bench/play.lua),
# trips on Script/Bench/trip.lua.
#
# -n N: each flight N times (<name>-r<k>.ft), for the run-to-run spread
# (compare.py uses it as the noise floor).
# -F: fixed steps for A/B comparisons: real-time flights run with
# --fixedstep=1/60 and ORBITER_VK_FRAMETIME (frame for frame the same
# simulation whatever the frame rate: hitches don't change what follows);
# flights with fixed steps of their own keep them.
#
# The scenarios are copied to out_dir/scn with their scripts taken from this
# source tree (Script/Bench), whichever build runs them; the run's stamp has
# the hashes of the scenario, its script and the source tree's diff.
# Per flight in out_dir: <name>.ft (ORBITER_VK_FRAMETIMES: a header with the
# device, settings and this run's stamp, then per frame: sim time, wall ms,
# GPU ms and passes, CPU sections, bound, workload counters, heap use, GPU
# clocks) and <name>.log; run.json (the build and the run's settings); at the
# end summary.json and summary.txt by
#   python3 Tests/Bench/analyze.py -j out_dir/summary.json out_dir/*.ft
# The measured window starts at the script's "bench: measure" log line.
# Compare two runs: python3 Tests/Bench/compare.py base_dir new_dir
# Orbiter.cfg is backed up and restored; the window size and
# ReplayTimewarp = FALSE (playbacks at the script's warp) set for the runs.
# Each run waits for 6 GB of free memory (MemAvailable). Run all of it under the
# machine's benchmark lock if others share it.
set -u
SRC=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$SRC/out/build/linux-x64-headless-release
OUT=$PWD/bench-out
RES=2560x1440
REPEAT=1
FIXED=0
while getopts "b:o:r:n:F" o; do case $o in
	b) BUILD=$(cd "$OPTARG" && pwd) ;; o) OUT=$OPTARG ;; r) RES=$OPTARG ;; n) REPEAT=$OPTARG ;; F) FIXED=1 ;; *) exit 2 ;;
esac; done
shift $((OPTIND - 1))
mkdir -p "$OUT/scn"; OUT=$(cd "$OUT" && pwd)
GIT=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)
DIRTY=$( [ -n "$(git -C "$SRC" status --porcelain -uno 2>/dev/null)" ] && echo 1 || echo 0)
DIFF=$(git -C "$SRC" diff HEAD 2>/dev/null | sha1sum | cut -c1-10)
MODE=$( [ "$FIXED" = 1 ] && echo fixed || echo realtime)
FLIGHTS=${@:-Atmos Ascent Moon Mars Jupiter Proxima Barnard Nyx}
FLIGHTS=${FLIGHTS/quick/AtmosQ LaunchQ DGHover Dock NightCity Crash}
FLIGHTS=${FLIGHTS/pacing/CamCuts CamToggle ColdStart}
FLIGHTS=${FLIGHTS/heavy/HighWarp ManyVessels Splashdown Reentry CloudDeck}
FLIGHTS=${FLIGHTS/nightly/Soak}
FLIGHTS=${FLIGHTS/matrix/Matrix}
FLIGHTS=${FLIGHTS/all/Atmos Ascent Moon Mars Jupiter Proxima Barnard Nyx Launch LaunchNight LaunchFar Crash DGHover Dock NightCity CamCuts CamToggle ColdStart HighWarp ManyVessels Splashdown Reentry CloudDeck}
cat > "$OUT/run.json" <<JSON
{"date": "$(date -Iseconds)", "git": "$GIT", "dirty": $DIRTY, "diff_hash": "$DIFF", "mode": "$MODE", "size": "$RES", "build": "$BUILD",
 "kernel": "$(uname -r)", "flights": "$FLIGHTS", "repeat": $REPEAT}
JSON
cd "$BUILD" || exit 1
cp Orbiter.cfg "$OUT/Orbiter.cfg.orig"
trap 'cp "$OUT/Orbiter.cfg.orig" "$BUILD/Orbiter.cfg"' EXIT # (absolute: the summary runs from the source tree)
benchcfg() { # (the window size $1x$2)
	grep -v "^WindowWidth\|^WindowHeight\|^ReplayTimewarp" "$OUT/Orbiter.cfg.orig" > "$OUT/Orbiter.cfg.bench"
	printf 'ReplayTimewarp = FALSE\nWindowWidth = %s\nWindowHeight = %s\n' "$1" "$2" >> "$OUT/Orbiter.cfg.bench"
}
benchcfg "${RES%x*}" "${RES#*x}"
ulimit -v 12000000

# the scenario with its script from this source tree (relative to the build's Script folder)
scenario() { local scn=$1
	python3 - "$SRC/Tests/Bench/$scn.scn" "$OUT/scn/$scn.scn" "$BUILD/Script" "$SRC/Script" <<'PY'
import os, re, sys
src, dst, bscript, sscript = sys.argv[1:]
t = open(src).read()
def fix(m):
    rel = os.path.relpath(os.path.join(sscript, m.group(2)), bscript)
    return m.group(1) + rel
open(dst, "w").write(re.sub(r"(?m)^(\s*Script\s+)(\S+)\s*$", fix, t))
PY
}
hash() { [ -f "$1" ] && sha1sum "$1" | cut -c1-10 || echo none; }
memwait() { # (6 GB free, waiting up to 10 minutes)
	local i m
	for i in $(seq 1 60); do
		m=$(awk '/MemAvailable/ {print int($2/1048576)}' /proc/meminfo)
		[ "$m" -ge 6 ] && return 0
		[ "$i" = 1 ] && echo "waiting for memory (MemAvailable $m GB < 6)"
		sleep 10
	done
	return 1
}

run() { local base=${1%@*} scn=${1#*@} tmo=$2; shift 2
	local k name args=${ARGS:-} fixenv=
	if [ "$FIXED" = 1 ] && [[ "$args" != *fixedstep* ]]; then
		args="$args --fixedstep=0.0166667"; fixenv=ORBITER_VK_FRAMETIME=0.0166667
	fi
	scenario "$scn"
	local lua=$(sed -n 's/^\s*Script\s\+\(\S\+\)\s*$/\1/p' "$SRC/Tests/Bench/$scn.scn" | head -1)
	local sh=$(hash "$SRC/Tests/Bench/$scn.scn") lh=$(hash "$SRC/Script/$lua.lua")
	for k in $(seq 1 "$REPEAT"); do
		name=$base; [ "$REPEAT" -gt 1 ] && name=$base-r$k
		memwait || { echo "$name: skipped (no memory)"; continue; }
		[ -n "${PRERUN:-}" ] && eval "$PRERUN" # (per run: e.g. ColdStart's empty shader cache)
		cp "$OUT/Orbiter.cfg.bench" Orbiter.cfg
		local t0=$(date +%s)
		# (the stamp: shlex-quoted values in the frame log's header)
		local envs=$(printf '%s;' "$@" $fixenv | tr '"' "'")
		env "$@" $fixenv ORBITER_VK_OFFSCREEN=1 ORBITER_VK_PROFILE=1 ORBITER_VK_FRAMETIMES="$OUT/$name.ft" \
			ORBITER_BENCH_STAMP="git=$GIT dirty=$DIRTY diff_hash=$DIFF mode=$MODE flight=$base scenario=$scn scn_hash=$sh lua_hash=$lh args=\"$args\" env=\"$envs\"" \
			timeout "$tmo" ./Orbiter -p VulkanClient "--scenariox=$OUT/scn/$scn.scn" $args >/dev/null 2>&1
		local rc=$?
		cp Orbiter.log "$OUT/$name.log"
		echo "$name rc=$rc wall=$(( $(date +%s) - t0 ))s frames=$(grep -vc '^#' "$OUT/$name.ft") $(grep -a 'bench' Orbiter.log | grep -v measure | tail -1 | cut -c13-)"
	done
}

matrix() { # (NightCity, DGHover, AtmosQ: sizes x anti-aliasing x ray tracing, fixed steps, one run each)
	local f r aa rt
	local saveF=$FIXED; FIXED=1
	for f in NightCity DGHover AtmosQ; do
		for r in 1920x1080 2560x1440 3840x2160; do
			benchcfg "${r%x*}" "${r#*x}"
			for aa in taa fsr3 fsr4; do
				for rt in 1 0; do
					case $f in
					NightCity) run M-$f-${r#*x}-$aa-rt$rt@NightCity 600 BENCH_END=20 ORBITER_VK_UPSCALER=$aa ORBITER_VK_RT=$rt ;;
					DGHover)   run M-$f-${r#*x}-$aa-rt$rt@DGHover 600 BENCH_END=25 ORBITER_VK_UPSCALER=$aa ORBITER_VK_RT=$rt ;;
					AtmosQ)    run M-$f-${r#*x}-$aa-rt$rt@Atmos 600 BENCH_SCHED="0:1" BENCH_END=45 ORBITER_VK_UPSCALER=$aa ORBITER_VK_RT=$rt ;;
					esac
				done
			done
		done
	done
	benchcfg "${RES%x*}" "${RES#*x}"
	FIXED=$saveF
}

for n in $FLIGHTS; do case $n in
	Atmos)   run Atmos 1500 BENCH_SCHED="0:1" BENCH_END=335 ;;
	Ascent)  run Ascent 2700 BENCH_SCHED="0:1,240:10,415:1,470:10,580:1,649:10,1102:100,1800:20,3110:10,3443:50,5290:100,29000:10" BENCH_END=29395 ;;
	Moon)    run Moon 1500 BENCH_SCHED="0:20,165:500,14415:20,15405:30,15965:20,16165:30,16870:20,17235:100,19250:20,19545:5000,281090:20,283650:500,304155:20,304580:50,307380:20" BENCH_END=308835 ;;
	Mars)    run Mars 1500 TRIP_TARGET=Mars TRIP_ARR=orbit TRIP_ALT=500e3 TRIP_G=1000 TRIP_WARP=100 ;;
	Jupiter) run Jupiter 1500 TRIP_TARGET=Jupiter TRIP_ARR=orbit TRIP_ALT=50e6 TRIP_G=1000 TRIP_WARP=100 ;;
	Proxima) run Proxima 1800 "TRIP_TARGET=Proxima b" TRIP_ARR=orbit TRIP_ALT=1000e3 TRIP_G=1 TRIP_SHIPWARP=1e6 ;;
	Barnard) run Barnard 1800 "TRIP_TARGET=Barnard b" TRIP_ARR=orbit TRIP_ALT=1000e3 TRIP_G=1 TRIP_SHIPWARP=1e6 ;;
	Nyx)     run Nyx 2700 TRIP_TARGET=Nyx TRIP_ARR=rest TRIP_ALT=default TRIP_G=1 TRIP_SHIPWARP=1e6 TRIP_WALLMAX=2400 ;;
	Launch)    ARGS=--fixedstep=0.02 run Launch 900 BENCH_END=60 ORBITER_VK_PARTICLE_DENSITY=4 ORBITER_VK_PARTICLE_STATS=5 ORBITER_VK_FRAMETIME=0.016 ;;
	Launch8)   ARGS=--fixedstep=0.02 run Launch8@Launch 900 BENCH_END=60 ORBITER_VK_PARTICLE_DENSITY=8 ORBITER_VK_PARTICLE_STATS=5 ORBITER_VK_FRAMETIME=0.016 ;;
	LaunchNight) ARGS=--fixedstep=0.02 run LaunchNight 900 BENCH_END=60 ORBITER_VK_PARTICLE_DENSITY=4 ORBITER_VK_PARTICLE_STATS=5 ORBITER_VK_FRAMETIME=0.016 ;;
	LaunchFar) ARGS=--fixedstep=0.02 run LaunchFar 1200 BENCH_END=240 ORBITER_VK_PARTICLE_DENSITY=4 ORBITER_VK_PARTICLE_STATS=5 ORBITER_VK_FRAMETIME=0.016 ;;
	Crash)     ARGS=--fixedstep=0.0333 run Crash 600 BENCH_AFTER=12 ORBITER_DAMAGE=1 ORBITER_VK_PARTICLE_DENSITY=4 ORBITER_VK_PARTICLE_STATS=5 ORBITER_VK_FRAMETIME=0.016 ;;
	AtmosQ)    run AtmosQ@Atmos 600 BENCH_SCHED="0:1" BENCH_END=90 ;;
	LaunchQ)   ARGS=--fixedstep=0.02 run LaunchQ@Launch 600 BENCH_END=30 ORBITER_VK_PARTICLE_DENSITY=4 ORBITER_VK_PARTICLE_STATS=5 ORBITER_VK_FRAMETIME=0.016 ;;
	DGHover)   run DGHover 600 BENCH_END=60 ;;
	Dock)      run Dock 600 BENCH_END=50 BENCH_SWITCH=25 ;;
	NightCity) run NightCity 600 BENCH_END=45 ;;
	CamCuts)   run CamCuts 600 BENCH_END=64 BENCH_HOLD=4 ;;
	CamToggle) run CamToggle@CamCuts 600 BENCH_END=24 BENCH_HOLD=1 BENCH_VIEWS=1,3 BENCH_FIXED=1 ;;
	ColdStart) PRERUN='rm -rf "$OUT/mesa-cold"; mkdir -p "$OUT/mesa-cold"' \
	           run ColdStart@NightCity 600 BENCH_END=10 MESA_SHADER_CACHE_DIR="$OUT/mesa-cold" ;;
	HighWarp)  run HighWarp 600 BENCH_SCHED="0:1,10:100,20:1000,30:10000" BENCH_END=40 ;;
	ManyVessels) run ManyVessels 600 BENCH_SPEED=400 BENCH_PAN=20 ;;
	Splashdown) run Splashdown 600 BENCH_AFTER=12 ORBITER_DAMAGE=1 ;;
	Reentry)   run Reentry 600 BENCH_END=60 BENCH_SWITCH=30 ;;
	CloudDeck) run CloudDeck 600 BENCH_END=90 ;;
	Soak)      run Soak 2400 BENCH_END=1800 BENCH_CUT=30 BENCH_BURST=120 ;;
	Matrix)    matrix ;;
	*) echo "unknown flight $n" ;;
esac; done
cd "$SRC"
python3 Tests/Bench/analyze.py -j "$OUT/summary.json" $(ls "$OUT"/*.ft) | tee "$OUT/summary.txt"
