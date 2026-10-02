#!/bin/sh
# Tunes blocking/setup weights by paired games: each (blocking, setup) grid
# point plays the reference parameters head to head (both players
# static-ish, see bsstudy:games), every point on the same seed so points
# are compared on the same tile sequences. Run from the repository root
# with an optimized test binary:
#
#   test/blocking_setup/tune_weights.sh <lexicon> <reference.bsp> \
#       <blocking_weights> <setup_weights> <pairs> <seed> <out_dir> \
#       [workers] [racks] [z]
#
# Weights are comma-separated lists; the reference's own point is skipped.
# Each point's file is the reference with its weight rows replaced, so the
# teacher settings (teacher_value, racks) stay the same. Picking the best
# of several noisy points overstates it: confirm the winner on a fresh seed.
set -eu
lex="$1"; reference="$2"; blocking_list="$3"; setup_list="$4"
pairs="$5"; seed="$6"; out="$7"
workers="${8:-8}"; racks="${9:-64}"; z="${10:-3}"
bin="${BIN:-./bin/magpie_test}"
mkdir -p "$out"
ref_blocking=$(grep '^blocking_weight,' "$reference" | cut -d, -f2)
ref_setup=$(grep '^setup_weight,' "$reference" | cut -d, -f2)
{
  echo "lexicon=$lex reference=$reference pairs=$pairs seed=$seed"
  echo "blocking=$blocking_list setup=$setup_list racks=$racks z=$z"
  echo "reference_sha256=$(shasum -a 256 "$reference" | cut -d' ' -f1)"
  echo "binary_sha256=$(shasum -a 256 "$bin" | cut -d' ' -f1)"
  echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$out/MANIFEST.txt"
for blocking in $(echo "$blocking_list" | tr ',' ' '); do
  for setup in $(echo "$setup_list" | tr ',' ' '); do
    if [ "$(echo "$blocking == $ref_blocking && $setup == $ref_setup" | bc)" = 1 ]; then
      continue
    fi
    point="$out/b${blocking}_s${setup}"
    if [ -e "$point/done" ]; then
      continue
    fi
    mkdir -p "$point"
    grep -v '^blocking_weight,\|^setup_weight,\|^model_version,' "$reference" \
      > "$point/params.bsp"
    {
      echo "model_version,tune-b${blocking}-s${setup}"
      echo "blocking_weight,$blocking"
      echo "setup_weight,$setup"
    } >> "$point/params.bsp"
    worker=0
    while [ "$worker" -lt "$workers" ]; do
      (rm -f settings.txt; "$bin" "bsstudy:games:lex=$lex:a=adjusted:b=adjusted:pairs=$pairs:seed=$seed:worker=$worker:workers=$workers:params=$point/params.bsp:params_b=$reference:racks=$racks:z=$z:out=$point/w$worker" > "$point/w$worker.log" 2>&1) &
      worker=$((worker + 1))
    done
    wait
    touch "$point/done"
    echo "[$(date +%H:%M:%S)] b=$blocking s=$setup done"
  done
done
echo "finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$out/MANIFEST.txt"
