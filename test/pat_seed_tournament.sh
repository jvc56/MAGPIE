#!/bin/sh
# Trains more seeds of the v5 recipe for a lexicon and races each against
# a reference file on that lexicon's own games, so the bar is "better
# than what we would otherwise ship" (seed draws differ by 0.2-0.4 per
# pair, more than the lexicon-specific signal, so a few draws are not
# enough).
#
#   test/pat_seed_tournament.sh <lexicon> <name> <first_seed_index> \
#       <num_seeds> <reference_pat> [<log_dir>]
#
# Seed index i trains with seed 61000002 + i - 1 into <name>_s<i>_v3 and
# refits into <name>_s<i>_v4 (see pat_champion_recipe.md), then plays
# 500K mirrored pairs as player 1 against <reference_pat>. Speed-only
# tables are built if absent and left in place (delete them once the
# lexicon's final file is validated). Results accumulate in
# <log_dir>/tournament.txt.
set -eu
if [ "$#" -lt 5 ]; then
  echo "usage: $0 <lexicon> <name> <first_seed_index> <num_seeds> <reference_pat> [<log_dir>]" >&2
  exit 2
fi
lex="$1"; name="$2"; first="$3"; count="$4"; reference="$5"
log_dir="${6:-/tmp/pat_seeds_$name}"
mkdir -p "$log_dir"
strategy=data/strategy
threads=10
train_seed_base=61000002
refit_seed=4242
match_seed_base=777300000
pairs=500000
tables=""
if [ ! -f "data/lexica/$lex.rit" ]; then
  echo "[$(date +%H:%M:%S)] building data/lexica/$lex.rit"
  echo "convert klvwmp2rit $lex" | ./bin/magpie "set -lex $lex -wmp true -rit false" > "$log_dir/build_rit.txt" 2>&1
fi
if [ ! -f "data/lexica/$lex.wit" ]; then
  echo "[$(date +%H:%M:%S)] building data/lexica/$lex.wit"
  echo "convert kwg2wit $lex" | ./bin/magpie "set -lex $lex -wit false" > "$log_dir/build_wit.txt" 2>&1
fi
[ -f "data/lexica/$lex.rit" ] && tables="$tables -rit true -ritmmap true"
[ -f "data/lexica/$lex.wit" ] && tables="$tables -wit true"

i="$first"
last=$((first + count - 1))
while [ "$i" -le "$last" ]; do
  seed=$((train_seed_base + i - 1))
  v3="${name}_s${i}_v3"
  v4="${name}_s${i}_v4"
  if [ ! -f "$strategy/$v4.pat" ]; then
    echo "[$(date +%H:%M:%S)] seed $i ($seed): training $v3"
    ./bin/magpie patgen 30000,30000,30000,30000,30000 "$v3" -lex "$lex" \
      -gp true -threads $threads -seed "$seed" -wmp true $tables \
      -pat pat_zero_lexsigned_nofit > "$log_dir/train_s$i.txt" 2>&1
    sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,0$/fit_residual,3/' \
      "$strategy/$v3.pat" > "$strategy/${v3}_runres.pat"
    echo "[$(date +%H:%M:%S)] seed $i: through refit $v4"
    ./bin/magpie patgen 150000 "$v4" -lex "$lex" -gp true -threads $threads \
      -seed $refit_seed -wmp true $tables -pat "${v3}_runres" > "$log_dir/refit_s$i.txt" 2>&1
    sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/$v4.pat"
  fi
  echo "[$(date +%H:%M:%S)] match seed $i vs $reference on $lex"
  ./bin/magpie autoplay games $pairs -lex "$lex" -gp true -threads $threads \
    -seed $((match_seed_base + i)) -wmp true $tables \
    -pat1 "$v4" -pat2 "$reference" > "$log_dir/match_s${i}_vs_ref.txt" 2>&1
  line="$(grep -m1 "mirrored pair" "$log_dir/match_s${i}_vs_ref.txt" | sed 's/Player 1 spread per mirrored pair: //')"
  echo "seed $i vs $reference: $line" | tee -a "$log_dir/tournament.txt"
  i=$((i + 1))
done
echo "[$(date +%H:%M:%S)] done; results in $log_dir/tournament.txt"
