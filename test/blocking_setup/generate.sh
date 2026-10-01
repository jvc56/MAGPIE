#!/bin/sh
# Generates blocking/setup training data for one lexicon: independent games,
# one position per game, the pass-relative teacher at several rack counts,
# and two independent reference simulations per position. Run from the
# repository root with an optimized test binary
# (make magpie_test BUILD=no_pgo_release):
#
#   test/blocking_setup/generate.sh <lexicon> <out_dir> <games> <seed> \
#       [workers] [ref_ms] [pat]
#
# Seeds: games use <seed>, the teacher <seed>+1, references <seed>+2 and
# <seed>+3, so each stage's randomness is independent of the others. The
# opponent's rack in each saved CGP is never read by the teacher or the
# features. Outputs land in <out_dir> (never commit them); fit with
# test/blocking_setup/bs_fit.py.
set -eu
lex="$1"; out="$2"; games="$3"; seed="$4"
workers="${5:-8}"; ref_ms="${6:-10000}"; pat="${7:-}"
leaves="${LEAVES:-$lex}"
racks="${RACKS:-16,64,256}"
plies="${PLIES:-4}"
wmp="${WMP:-true}"
mkdir -p "$out"
if [ -e "$out/positions.csv" ]; then
  echo "$out/positions.csv exists; use a fresh directory" >&2
  exit 1
fi
# BIN: a copy of the test binary, so rebuilding mid-run cannot mix builds.
bin="${BIN:-./bin/magpie_test}"
mgt() { rm -f settings.txt; "$bin" "$1" > /dev/null; }
common="lex=$lex:leaves=$leaves:wmp=$wmp"
log() { echo "[$(date +%H:%M:%S)] $lex: $*"; }

{
  echo "lexicon=$lex leaves=$leaves games=$games seed=$seed workers=$workers"
  echo "racks=$racks ref_ms=$ref_ms plies=$plies wmp=$wmp pat=$pat"
  echo "binary_sha256=$(shasum -a 256 "$bin" | cut -d' ' -f1)"
  echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$out/MANIFEST.txt"
log "positions ($games games)"
worker=0
while [ "$worker" -lt "$workers" ]; do
  mgt "bsgen:positions:$common:games=$games:seed=$seed:worker=$worker:workers=$workers:out=$out/positions.w$worker.csv" &
  worker=$((worker + 1))
done
wait
{
  echo "game,turn,bag,lead,phase,cgp"
  for file in "$out"/positions.w*.csv; do tail -n +2 "$file"; done
} | sort -t, -k1,1n > "$out/positions.unsorted"
# The header sorts first ("game" is not a number); keep it there.
grep '^game,' "$out/positions.unsorted" > "$out/positions.csv"
grep -v '^game,' "$out/positions.unsorted" >> "$out/positions.csv"
rm -f "$out/positions.unsorted" "$out"/positions.w*.csv

pat_arg=""
if [ -n "$pat" ]; then pat_arg=":pat=$pat"; fi
log "labels (racks $racks)"
worker=0
while [ "$worker" -lt "$workers" ]; do
  mgt "bsgen:labels:$common$pat_arg:racks=$racks:seed=$((seed + 1)):in=$out/positions.csv:worker=$worker:workers=$workers:out=$out/labels.w$worker.csv" &
  worker=$((worker + 1))
done
wait

for ref in A B; do
  if [ "$ref" = A ]; then ref_seed=$((seed + 2)); else ref_seed=$((seed + 3)); fi
  log "reference $ref (${ref_ms} ms, $plies plies)"
  worker=0
  while [ "$worker" -lt "$workers" ]; do
    mgt "bsgen:refs:$common:plies=$plies:ms=$ref_ms:seed=$ref_seed:in=$out/positions.csv:labels=$out/labels.w$worker.csv:worker=$worker:workers=$workers:out=$out/refs$ref.w$worker.csv" &
    worker=$((worker + 1))
  done
  wait
done
echo "finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$out/MANIFEST.txt"
log "done"
