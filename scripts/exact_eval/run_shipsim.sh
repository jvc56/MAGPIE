#!/bin/sh
# WIP measurement driver for the exact + linear sim test (not for merge).
# Set up: make magpie_test BUILD=no_pgo_release; mkdir <out>;
#   cp bin/magpie_test <out>/mt; cp scripts/exact_eval/{run_shipsim.sh,tasks.py,analyze.py} <out>/
# usage, from the repository root (needs ./data with CSW24 .kwg .klv2 .wmp .rvt):
#   <out>/run_shipsim.sh <first game> <blocks> <processes>
# Each block: 84 fresh games, picks at 128,000 iterations with -poolshift 1.2
# and with exact, then the endbag=14 judge on the picks that differ. Finished
# blocks are skipped on a rerun.
# Analyse: cd <out>; python3 analyze.py . shiftend block1 block2 ...
OUT=$(cd "$(dirname "$0")" && pwd)
FIRST=$1; BLOCKS=$2; PROCS=$3
k=1
while [ "$k" -le "$BLOCKS" ]; do
  D="$OUT/block$k"
  if [ ! -f "$D/done" ]; then
    mkdir -p "$D"
    echo "$(date) block $k picks" >> "$OUT/log"
    "$OUT/mt" "simexact:first=$((FIRST + 84 * (k - 1))),games=84,static=0" 2>/dev/null | grep --line-buffered "^SIMX" > "$D/simx.out"
    python3 "$OUT/tasks.py" "$D/simx.out" "$D/nest_tasks.tsv" shift exact >> "$OUT/log"
    echo "$(date) block $k judge" >> "$OUT/log"
    i=0
    while [ "$i" -lt "$PROCS" ]; do
      "$OUT/mt" "simnest:tasks=$D/nest_tasks.tsv,mode=1.2,slice=$i,of=$PROCS,samples=100,iters=5000,endbag=14" 2>/dev/null | grep --line-buffered "^NEST" > "$D/nest_shiftend_$i.out" &
      i=$((i + 1))
    done
    wait
    touch "$D/done"
    echo "$(date) block $k done" >> "$OUT/log"
  fi
  k=$((k + 1))
done
echo "$(date) finished" >> "$OUT/log"
