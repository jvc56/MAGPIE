#!/bin/sh
# Builds a PAT champion file for a lexicon by the v5 recipe
# (notes/pat_champion_recipe.md): NUM_SEEDS iterative trainings, each
# followed by the run-keyed-through residual refit; a whole-game
# tournament of the seeds against the first; the opening-table sim on
# the winner; the final file assembled with its provenance in the header;
# and a validation match against no PAT.
#
#   test/pat_build_champion.sh <lexicon> <output_name> [<num_seeds>] [<log_dir>]
#
# Run from the repository root with bin/magpie and bin/magpie_test built
# (no_pgo_release and vlg). Intermediate files are <output_name>_s<i>_v3,
# _s<i>_v3_runres and _s<i>_v4 in data/strategy; the result is
# data/strategy/<output_name>.pat.
set -eu
if [ "$#" -lt 2 ]; then
  echo "usage: $0 <lexicon> <output_name> [<num_seeds>] [<log_dir>]" >&2
  exit 2
fi
lex="$1"
name="$2"
num_seeds="${3:-4}"
log_dir="${4:-/tmp/pat_build_$name}"
mkdir -p "$log_dir"
strategy=data/strategy
threads=10
train_seed_base=61000002
refit_seed=4242
match_seed_base=777100000
pairs=500000

# Production files never carry mass on the experimental channels: a
# default fit holds them at their loaded value, which must be zero here.
assert_experimental_channels_zero() {
  bad="$(grep -E '^(hook_score_d[0-9]+|lm_span_d[0-9]+|lm_ext_d[0-9]+|dws_lm_span_d[0-9]+|dws_lm_ext_d[0-9]+),' "$1" | grep -v ',0$' || true)"
  if [ -n "$bad" ]; then
    echo "ERROR: $1 carries weight on experimental channels:" >&2
    echo "$bad" >&2
    exit 3
  fi
}
opening_racks=1000
train_games=30000,30000,30000,30000,30000
refit_games=150000

# Speed-only tables: the rack info table (leave values per full rack)
# and the word info table (subrack pruning). Neither changes a move
# choice; they only shorten the run. Built here when the lexicon lacks
# them, and deleted at the end -- the RIT is about 1.8 GB -- only when
# this run built them AND the validation match below clears
# DELETE_TABLES_MIN_LOWER_CI per pair (a shippable file), never a table
# that was already there.
tables="-rit true -ritmmap true -wit true"
delete_tables_min_lower_ci=2.0
built_rit=0
built_wit=0
if [ ! -f "data/lexica/$lex.rit" ]; then
  echo "[$(date +%H:%M:%S)] building data/lexica/$lex.rit"
  echo "convert klvwmp2rit $lex" | ./bin/magpie "set -lex $lex -wmp true -rit false" \
    > "$log_dir/build_rit.txt" 2>&1
  [ -f "data/lexica/$lex.rit" ]
  built_rit=1
fi
if [ ! -f "data/lexica/$lex.wit" ]; then
  echo "[$(date +%H:%M:%S)] building data/lexica/$lex.wit"
  echo "convert kwg2wit $lex" | ./bin/magpie "set -lex $lex -wit false" \
    > "$log_dir/build_wit.txt" 2>&1
  [ -f "data/lexica/$lex.wit" ]
  built_wit=1
fi

pair_mean() {
  # The per-pair mean of a finished autoplay match.
  grep -m1 "mirrored pair" "$1" | sed 's/.*mean \([-0-9.]*\),.*/\1/'
}

i=1
while [ "$i" -le "$num_seeds" ]; do
  seed=$((train_seed_base + i - 1))
  v3="${name}_s${i}_v3"
  v4="${name}_s${i}_v4"
  echo "[$(date +%H:%M:%S)] seed $i ($seed): training $v3"
  ./bin/magpie patgen "$train_games" "$v3" -lex "$lex" \
    -gp true -threads $threads -seed "$seed" -wmp true $tables \
    -pat pat_zero_lexsigned_nofit > "$log_dir/train_s$i.txt" 2>&1
  sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,0$/fit_residual,3/' \
    "$strategy/$v3.pat" > "$strategy/${v3}_runres.pat"
  grep -q '^run_through,1$' "$strategy/${v3}_runres.pat"
  grep -q '^fit_residual,3$' "$strategy/${v3}_runres.pat"
  echo "[$(date +%H:%M:%S)] seed $i: through refit $v4"
  ./bin/magpie patgen "$refit_games" "$v4" -lex "$lex" -gp true -threads $threads \
    -seed $refit_seed -wmp true $tables -pat "${v3}_runres" > "$log_dir/refit_s$i.txt" 2>&1
  sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/$v4.pat"
  assert_experimental_channels_zero "$strategy/$v3.pat"
  assert_experimental_channels_zero "$strategy/$v4.pat"
  i=$((i + 1))
done

# Tournament: every seed against the first, whole game.
best=1
best_mean=0
tournament=""
i=2
while [ "$i" -le "$num_seeds" ]; do
  echo "[$(date +%H:%M:%S)] match seed $i vs seed 1"
  ./bin/magpie autoplay games $pairs -lex "$lex" -gp true -threads $threads \
    -seed $((match_seed_base + i)) -wmp true $tables \
    -pat1 "${name}_s${i}_v4" -pat2 "${name}_s1_v4" > "$log_dir/match_s${i}_vs_s1.txt" 2>&1
  line="$(grep -m1 "mirrored pair" "$log_dir/match_s${i}_vs_s1.txt")"
  mean="$(pair_mean "$log_dir/match_s${i}_vs_s1.txt")"
  echo "  seed $i vs seed 1: $line"
  tournament="$tournament#   seed $i vs seed 1: ${line#Player 1 spread per mirrored pair: }
"
  if [ "$(echo "$mean > $best_mean" | bc)" -eq 1 ]; then
    best=$i
    best_mean=$mean
  fi
  i=$((i + 1))
done
winner="${name}_s${best}_v4"
echo "[$(date +%H:%M:%S)] winner: seed $best ($winner)"

# Opening table on the winner (a file without one).
echo "[$(date +%H:%M:%S)] opening sim on $winner"
./bin/magpie_test "patopeningsim:$lex:$winner:$opening_racks" > "$log_dir/opening_sim.txt" 2>&1
rows="$(grep -E '^opening_(tiles_[0-9]+|exchange),-?[0-9]+$' "$log_dir/opening_sim.txt")"
[ -n "$rows" ]
echo "$rows"

# Assemble the champion: the winner's rows with the opening rows filled in.
{
  head -1 "$strategy/$winner.pat"
  echo "# $name: PAT champion for $lex by the v5 recipe"
  echo "# (notes/pat_champion_recipe.md, codex/pat-setup-value). Weights: the"
  echo "# best of $num_seeds seeds ($((train_seed_base)) + i - 1) of the iterative"
  echo "# recipe (5 x 30K games from pat_zero_lexsigned_nofit) with the 14"
  echo "# float_through_* weights refitted under run_through,1 (150K games,"
  echo "# seed $refit_seed, fit_residual 3 during that fit). Tournament, whole game,"
  echo "# $pairs mirrored pairs, per pair:"
  printf '%s' "$tournament"
  echo "#   winner: seed $best"
  echo "# Opening table: sim - static by tiles on $opening_racks seeded opening racks"
  echo "# (patopeningsim:$lex:$winner:$opening_racks), relative to the best bin."
  echo "# Validation vs no PAT: see the row below once appended."
  # The winner's rows after its header line, with the zero opening rows
  # replaced by the measured ones.
  tail -n +2 "$strategy/$winner.pat" | grep -v '^#' | grep -v '^opening_'
  echo "$rows"
} > "$strategy/$name.pat"

# Validation against no PAT.
echo "[$(date +%H:%M:%S)] validation $name vs none"
./bin/magpie autoplay games $pairs -lex "$lex" -gp true -threads $threads \
  -seed $((match_seed_base + 99)) -wmp true $tables -pat1 "$name" -pat2 none \
  > "$log_dir/validation_vs_none.txt" 2>&1
line="$(grep -m1 "mirrored pair" "$log_dir/validation_vs_none.txt")"
echo "  $name vs none: $line"
sed -i '' "s|^# Validation vs no PAT: see the row below once appended.|# Validation vs no PAT (seed $((match_seed_base + 99)), $pairs pairs): ${line#Player 1 spread per mirrored pair: }|" "$strategy/$name.pat"
echo "[$(date +%H:%M:%S)] done: $strategy/$name.pat"

# Tables this run built go once the file is shippable: the validation's
# 95% interval must sit above delete_tables_min_lower_ci per pair (CSW21
# is about +3 over no PAT; the broken hook-score build was +0.3 to +1.2).
lower_ci="$(printf '%s' "$line" | sed 's/.*95% CI \[\([-0-9.]*\),.*/\1/')"
if [ "$(echo "$lower_ci > $delete_tables_min_lower_ci" | bc)" -eq 1 ]; then
  if [ "$built_rit" -eq 1 ]; then
    rm -f "data/lexica/$lex.rit" && echo "deleted data/lexica/$lex.rit (built by this run; validation lower CI $lower_ci)"
  fi
  if [ "$built_wit" -eq 1 ]; then
    rm -f "data/lexica/$lex.wit" && echo "deleted data/lexica/$lex.wit (built by this run; validation lower CI $lower_ci)"
  fi
else
  echo "validation lower CI $lower_ci does not clear $delete_tables_min_lower_ci: NOT shippable as is; tables kept"
fi
