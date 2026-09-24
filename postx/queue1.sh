#!/bin/bash
# Queue 1: seed-bagging whole-game tests vs X, then live-isolation refit.
cd "$(dirname "$0")/.."
true
T='-pat hookscore_x -lex CSW21 -gp true -threads 10 -wmp true -rit true -ritmmap true -wit true'
run() { tag=$1; shift; s=$(date +%s); ./bin/magpie_base autoplay games "$@" $T > postx/games/$tag.log 2>&1; e=$(date +%s); echo "$tag secs=$((e-s))" >> postx/games/$tag.log; echo "== $tag ($((e-s))s)"; grep -m1 "spread per mirrored" postx/games/$tag.log; }
run bag8_vs_x 1000000 -seed 93100001 -pat1 px_bag8 -pat2 hookscore_x
run bagx_vs_x 1000000 -seed 93100002 -pat1 px_bagx -pat2 hookscore_x
s=$(date +%s)
./bin/magpie_isolive patgen 150000 px_isolive_fit -lex CSW21 -gp true -threads 10 -seed 92600003 -wmp true -pat px_isolive_input > postx/games/isolive_train.log 2>&1
echo "== isolive train $(( $(date +%s)-s ))s"
for f in data/strategy/px_isolive_fit_gen_1_shrink*.pat; do echo "$f $(grep -E '^(plain|isolated)_hook' $f | tr '\n' ' ')"; done
