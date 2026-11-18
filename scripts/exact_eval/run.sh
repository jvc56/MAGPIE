#!/bin/sh
# WIP measurement driver for exact + shift leaves in sims (not for merge).
# usage: run.sh <out dir> <first game> <blocks> <processes>
# Run from the repository root after: make magpie_test BUILD=no_pgo_release
# Each block is 84 fresh static self-play games: both sims pick at 128,000
# iterations, then two nested judges value the picks that differ.
# Analyse with: python3 scripts/exact_eval/analyze.py <out dir> shift|public block1 block2 ...
OUT=$1
FIRST=$2
BLOCKS=$3
PROCS=$4
HERE=$(dirname "$0")
mkdir -p "$OUT"
cp bin/magpie_test "$OUT/mt"
nest() { # dir mode tag samples
  i=0
  while [ "$i" -lt "$PROCS" ]; do
    "$OUT/mt" "simnest:tasks=$1/nest_tasks.tsv,mode=$2,slice=$i,of=$PROCS,samples=$4,iters=5000" 2>/dev/null | grep --line-buffered "^NEST" > "$1/nest_$3_$i.out" &
    i=$((i + 1))
  done
  wait
}
k=1
while [ "$k" -le "$BLOCKS" ]; do
  D="$OUT/block$k"
  mkdir -p "$D"
  echo "$(date) block $k picks" >> "$OUT/log"
  "$OUT/mt" "simexact:first=$((FIRST + 84 * (k - 1))),games=84,static=0" 2>/dev/null | grep --line-buffered "^SIMX" > "$D/simx.out"
  python3 "$HERE/tasks.py" "$D/simx.out" "$D/nest_tasks.tsv" shift exact >> "$OUT/log"
  echo "$(date) block $k shift judge" >> "$OUT/log"
  nest "$D" 1.2 shift 100
  echo "$(date) block $k public judge" >> "$OUT/log"
  nest "$D" exactroot public 100
  echo "$(date) block $k done" >> "$OUT/log"
  k=$((k + 1))
done
echo "$(date) finished" >> "$OUT/log"
