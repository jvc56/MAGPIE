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
opening_racks=1000
train_games=30000,30000,30000,30000,30000
refit_games=150000

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
    -gp true -threads $threads -seed "$seed" -wmp true \
    -pat pat_zero_lexsigned_nofit > "$log_dir/train_s$i.txt" 2>&1
  sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,0$/fit_residual,3/' \
    "$strategy/$v3.pat" > "$strategy/${v3}_runres.pat"
  grep -q '^run_through,1$' "$strategy/${v3}_runres.pat"
  grep -q '^fit_residual,3$' "$strategy/${v3}_runres.pat"
  echo "[$(date +%H:%M:%S)] seed $i: through refit $v4"
  ./bin/magpie patgen "$refit_games" "$v4" -lex "$lex" -gp true -threads $threads \
    -seed $refit_seed -wmp true -pat "${v3}_runres" > "$log_dir/refit_s$i.txt" 2>&1
  sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/$v4.pat"
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
    -seed $((match_seed_base + i)) -wmp true \
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
  -seed $((match_seed_base + 99)) -wmp true -pat1 "$name" -pat2 none \
  > "$log_dir/validation_vs_none.txt" 2>&1
line="$(grep -m1 "mirrored pair" "$log_dir/validation_vs_none.txt")"
echo "  $name vs none: $line"
sed -i '' "s|^# Validation vs no PAT: see the row below once appended.|# Validation vs no PAT (seed $((match_seed_base + 99)), $pairs pairs): ${line#Player 1 spread per mirrored pair: }|" "$strategy/$name.pat"
echo "[$(date +%H:%M:%S)] done: $strategy/$name.pat"
