#!/bin/bash
# Sharded decision diagnostic: run_decide.sh <lex> <patA> <patB> <seed> <positions> <worlds> <tag>
set -e
cd "$(dirname "$0")/.."
lex=$1; a=$2; b=$3; seed=$4; n=$5; w=$6; tag=$7
out=postx/decide/$tag; mkdir -p "$out"; rm -f "$out"/shard*.txt
for s in $(seq 0 9); do
  ./bin/magpie_test "patdecide:$lex:$a:$b:$seed:$n:$w:$s:10" 2>/dev/null | grep -E '^(CASE|POS)' > "$out/shard$s.txt" &
done
wait
python3 postx/pool_decide.py "$out" "$tag"
