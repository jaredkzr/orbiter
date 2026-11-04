#!/bin/bash
# Poor man's profiler (perf may be unavailable: perf_event_paranoid; gdb
# can't attach to a running Orbiter: ptrace_scope 1): Orbiter run under gdb,
# interrupted every <gap> s (fractions too) after <delay> s, the main thread's stack printed.
#
#   Tests/Bench/sample.sh out.txt delay samples gap scenario.scn [VAR=value ...]
#
# SAMPLE_THREADS=all: every thread's stack (the workers too).
# Tally the functions with: python3 Tests/Bench/stacks.py out.txt
set -u
SRC=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$SRC/out/build/linux-x64-headless-release}
out=$(realpath -m "$1") delay=$2 ns=$3 gap=$4 scn=$(realpath "$5"); shift 5
cd "$BUILD" || exit 1
ulimit -v 12000000
cmds=$(mktemp)
{ echo "set pagination off"; echo "set print thread-events off"; echo "handle SIGINT stop nopass"; echo "run"
  for i in $(seq "$ns"); do echo "thread apply ${SAMPLE_THREADS:-1} bt 40"; echo "continue"; done; echo "kill"; echo "quit"; } > "$cmds"
( sleep "$delay"; for i in $(seq "$ns"); do p=$(pgrep -x Orbiter | head -1); [ -n "$p" ] && kill -INT "$p"; sleep "$gap"; done ) &
env "$@" ORBITER_VK_OFFSCREEN=1 timeout $(awk -v d="$delay" -v n="$ns" -v g="$gap" 'BEGIN { printf "%d", d + n * g + 120 }') gdb -q -batch -x "$cmds" --args ./Orbiter -p VulkanClient "--scenariox=$scn" > "$out" 2>&1
rm -f "$cmds"
