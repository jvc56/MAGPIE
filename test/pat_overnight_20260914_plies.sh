#!/bin/sh
# Takes over from run.sh after the finer stage scales: label-horizon
# experiments (2..5 plies), each judged by whole-game play first, since the
# 2-ply harness is horizon-biased on defense-strength questions.
cd /Users/olaugh/sources/magpie-pat-setup-value || exit 1
OUT=/tmp/overnight
SUM=$OUT/SUMMARY.txt
DATA=/Users/olaugh/sources/magpie-pr-20260910/data/strategy
log() { echo "$(date '+%H:%M') $*" >> $SUM; }
compare() { name=$1; spec=$2; shards=${3:-10}
  test/pat_move_choice_shard.sh "$spec" "$shards" "$OUT/$name" > "$OUT/$name.log" 2>&1
  log "$name: $(grep -h 'paired effect' $OUT/$name.txt 2>/dev/null | head -1) | $(grep -h 'disagreements' $OUT/$name.txt | head -1 | sed -E 's/.*considered, //')"; }
wholegame() { rm -f settings.txt
  ./bin/magpie autoplay games "$5" -lex CSW21 -gp true -threads 10 -seed "$4" -wmp true -pat1 "$2" -pat2 "$3" > "$OUT/$1.txt" 2>&1
  log "$1 (whole game, $5 pairs): $(grep -h 'mirrored pair' $OUT/$1.txt | head -1) | $(grep -h 'confidence' $OUT/$1.txt | head -1)"; }
until grep -q "stage_late_scale0.85_scale1.15\|overnight run finished" $SUM 2>/dev/null; do sleep 60; done
pkill -f "overnight/run.sh"; sleep 2; pkill -f "magpie_test patmovechoice"; pkill -f "magpie patgen"; sleep 3
log "run3: took over from run.sh (seeds 13-20 skipped)"
log "(side run) v3 x0.7 vs v3 whole game: $(grep -h 'mirrored pair' /tmp/wholegame_scaled07_vs_v3.txt 2>/dev/null | head -1)"
for p in 2 3 4 5; do
  rm -f settings.txt
  ./bin/magpie patgen 30000,30000,30000,30000,30000 pat_iter_lexsigned_leave_plies$p -lex CSW21 -gp true -threads 10 -seed 61000002 -wmp true -pat pat_zero_lexsigned_nofit -patplies $p > $OUT/patgen_plies$p.txt 2>&1
  log "plies$p trained: $(grep -E '^(hook_d1|hook_d2|float_score_d1|dd_floater),' $DATA/pat_iter_lexsigned_leave_plies$p.pat | tr '\n' ' ') fit/baseline MSE $(grep -hE 'Fit MSE|Baseline MSE' $DATA/pat_iter_lexsigned_leave_plies${p}_gen_5_report.txt | sed -E 's/.*: //' | tr '\n' '/')"
done
n=0
for p in 2 3 4 5; do
  n=$((n + 1))
  wholegame wholegame_plies${p}_vs_v3 pat_iter_lexsigned_leave_plies$p pat_dls_champion_v3 $((777000040 + n)) 500000
done
for p in 2 3 4 5; do
  compare plies${p}_vs_v3 "pat_dls_champion_v3:pat_iter_lexsigned_leave_plies$p:$((2000000000 + p * 10000000)):120000:100"
done
log "run3 finished"
