#!/bin/sh
# Paired games: player a against player b ("static", "pat" or "adjusted"),
# same tiles per seat in both games of a pair, seats swapped. Run from the
# repository root with an optimized test binary:
#
#   test/blocking_setup/games_study.sh <lexicon> <a> <b> <pairs> <out_dir> \
#       <seed> [workers] [params.bsp] [racks] [pat]
#
# Analyze with games_analyze.py.
set -eu
lex="$1"; a="$2"; b="$3"; pairs="$4"; out="$5"; seed="$6"
workers="${7:-8}"; params="${8:-}"; racks="${9:-}"; pat="${10:-}"
bin="${BIN:-./bin/magpie_test}"
mkdir -p "$out"
if ls "$out"/w*.games.csv > /dev/null 2>&1; then
  echo "$out already has results; use a fresh directory" >&2
  exit 1
fi
extra=""
if [ -n "$params" ]; then extra="$extra:params=$params"; fi
if [ -n "$racks" ]; then extra="$extra:racks=$racks"; fi
if [ -n "$pat" ]; then extra="$extra:pat=$pat"; fi
{
  echo "lexicon=$lex a=$a b=$b pairs=$pairs seed=$seed workers=$workers"
  echo "params=$params racks=$racks pat=$pat"
  if [ -n "$params" ]; then
    echo "params_sha256=$(shasum -a 256 "$params" | cut -d' ' -f1)"
  fi
  echo "binary_sha256=$(shasum -a 256 "$bin" | cut -d' ' -f1)"
  echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "hardware=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)"
  echo "started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$out/MANIFEST.txt"
worker=0
while [ "$worker" -lt "$workers" ]; do
  (rm -f settings.txt; "$bin" "bsstudy:games:lex=$lex:a=$a:b=$b:pairs=$pairs:seed=$seed:worker=$worker:workers=$workers:out=$out/w$worker$extra" > "$out/w$worker.log" 2>&1) &
  worker=$((worker + 1))
done
wait
echo "finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$out/MANIFEST.txt"
