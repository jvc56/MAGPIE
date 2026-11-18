#!/bin/sh
# WIP measurement driver (not for merge): judges the stored disagreements of
# finished blocks again, with the current test binary.
# usage: rejudge.sh <out dir> <mode: 1.2|exactroot> <tag> <samples> <processes> <block dirs...>
# Run from the repository root after: make magpie_test BUILD=no_pgo_release
# Writes <out dir>/<block>/nest_<tag>_<i>.out; analyse with
#   python3 scripts/exact_eval/analyze.py <out dir> <tag> block1 block2 ...
OUT=$1
MODE=$2
TAG=$3
SAMPLES=$4
PROCS=$5
shift 5
cp bin/magpie_test "$OUT/mt_$TAG"
for b in "$@"; do
  echo "$(date) rejudge $TAG $b" >> "$OUT/log"
  i=0
  while [ "$i" -lt "$PROCS" ]; do
    "$OUT/mt_$TAG" "simnest:tasks=$OUT/$b/nest_tasks.tsv,mode=$MODE,slice=$i,of=$PROCS,samples=$SAMPLES,iters=5000" 2>/dev/null | grep --line-buffered "^NEST" > "$OUT/$b/nest_${TAG}_$i.out" &
    i=$((i + 1))
  done
  wait
done
echo "$(date) rejudge $TAG finished" >> "$OUT/log"
