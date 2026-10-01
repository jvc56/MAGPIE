#!/bin/sh
# Root-pool comparison: arms of sim_nomination.h (A, B, Bp, C) against
# exactly equal-size static pools, top-two selection sims and an
# independent round-robin reference over the union, no-PAT static
# rollouts. Run from the repository root with an optimized test binary:
#
#   test/blocking_setup/pools_study.sh <lexicon> <pat> <params.bsp> \
#       <positions.csv> <out_dir> <seed> [workers] [select_ms] [ref_ms]
#
# positions.csv comes from generate.sh (or bsgen:positions) on games that
# were not used to fit params. Analyze with pools_analyze.py.
set -eu
lex="$1"; pat="$2"; params="$3"; positions="$4"; out="$5"; seed="$6"
workers="${7:-8}"; select_ms="${8:-15000}"; ref_ms="${9:-60000}"
arms="${ARMS:-A,B,Bp,C}"
bin="${BIN:-./bin/magpie_test}"
mkdir -p "$out"
if ls "$out"/w*.pools.csv > /dev/null 2>&1; then
  echo "$out already has results; use a fresh directory" >&2
  exit 1
fi
{
  echo "lexicon=$lex pat=$pat params=$params seed=$seed workers=$workers"
  echo "arms=$arms select_ms=$select_ms ref_ms=$ref_ms"
  echo "params_sha256=$(shasum -a 256 "$params" | cut -d' ' -f1)"
  echo "positions_sha256=$(shasum -a 256 "$positions" | cut -d' ' -f1)"
  echo "binary_sha256=$(shasum -a 256 "$bin" | cut -d' ' -f1)"
  echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$out/MANIFEST.txt"
worker=0
while [ "$worker" -lt "$workers" ]; do
  (rm -f settings.txt; "$bin" "bsstudy:pools:lex=$lex:pat=$pat:params=$params:in=$positions:out=$out/w$worker:arms=$arms:select_ms=$select_ms:ref_ms=$ref_ms:seed=$seed:worker=$worker:workers=$workers" > "$out/w$worker.log" 2>&1) &
  worker=$((worker + 1))
done
wait
echo "finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$out/MANIFEST.txt"
