#!/bin/sh
# Experimental v5 build with hook-score channels free in the full fit.
# Run from the repository root. Keeps the production builder untouched.
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 7 ]; then
  echo "usage: $0 <output_name> <log_dir> [train_seed_base] [match_seed_base] [exact_hooks_0_or_1] [games_per_iter_gen] [through_games]" >&2
  exit 2
fi
name="$1"
log_dir="$2"
train_seed_base="${3:-61000002}"
match_seed_base="${4:-777100000}"
exact_hooks="${5:-0}"
iter_games="${6:-30000}"
through_games="${7:-150000}"
case "$exact_hooks" in 0|1) ;; *) echo "exact_hooks must be 0 or 1" >&2; exit 2 ;; esac
for games in "$iter_games" "$through_games"; do
  case "$games" in ''|*[!0-9]*) echo "game counts must be positive integers" >&2; exit 2 ;; esac
  [ "$games" -gt 0 ] || { echo "game counts must be positive" >&2; exit 2; }
done
iter_schedule="$iter_games,$iter_games,$iter_games,$iter_games,$iter_games"
mkdir -p "$log_dir"
strategy=data/strategy
tables='-wmp true -rit true -ritmmap true -wit true'
lex=CSW21
threads=10
pairs=500000
awk -v exact="$exact_hooks" '{
  print
  if ($0 == "fit_scaled,0") {
    print "fit_residual,5"
    if (exact == 1) print "exact_created_hooks,1"
  }
}' "$strategy/pat_zero_lexsigned_nofit.pat" > "$strategy/${name}_bootstrap.pat"
grep -q '^fit_residual,5$' "$strategy/${name}_bootstrap.pat"
seed_index=1
while [ "$seed_index" -le 4 ]; do
  seed=$((train_seed_base + seed_index - 1))
  v3="${name}_s${seed_index}_v3"
  v4="${name}_s${seed_index}_v4"
  echo "seed $seed_index: iterative training"
  ./bin/magpie patgen "$iter_schedule" "$v3" \
    -lex "$lex" -gp true -threads "$threads" -seed "$seed" $tables \
    -pat "${name}_bootstrap" > "$log_dir/train_s${seed_index}.log" 2>&1
  sed -e 's/^run_through,0$/run_through,1/' \
      -e 's/^fit_residual,5$/fit_residual,3/' \
      "$strategy/$v3.pat" > "$strategy/${v3}_runres.pat"
  echo "seed $seed_index: through refit"
  ./bin/magpie patgen "$through_games" "$v4" -lex "$lex" -gp true \
    -threads "$threads" -seed 4242 $tables -pat "${v3}_runres" \
    > "$log_dir/refit_s${seed_index}.log" 2>&1
  sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/$v4.pat"
  seed_index=$((seed_index + 1))
done
best=1
best_mean=0
seed_index=2
while [ "$seed_index" -le 4 ]; do
  echo "seed $seed_index: whole-game screen"
  ./bin/magpie autoplay games "$pairs" -lex "$lex" -gp true \
    -threads "$threads" -seed "$((match_seed_base + seed_index))" $tables \
    -pat1 "${name}_s${seed_index}_v4" -pat2 "${name}_s1_v4" \
    > "$log_dir/s${seed_index}_vs_s1.log" 2>&1
  line=$(grep -m1 'Player 1 spread per mirrored pair' \
    "$log_dir/s${seed_index}_vs_s1.log")
  echo "$line"
  mean=$(printf '%s\n' "$line" | sed 's/.*mean \([-0-9.]*\),.*/\1/')
  if awk -v candidate="$mean" -v best="$best_mean" \
      'BEGIN {exit !(candidate > best)}'; then
    best="$seed_index"
    best_mean="$mean"
  fi
  seed_index=$((seed_index + 1))
done
winner="${name}_s${best}_v4"
echo "opening simulation: $winner"
./bin/magpie_test "patopeningsim:$lex:$winner:1000" \
  > "$log_dir/opening_sim.log" 2>&1
rows=$(grep -E '^opening_(tiles_[0-9]+|exchange),-?[0-9]+$' \
  "$log_dir/opening_sim.log")
[ -n "$rows" ]
{
  head -1 "$strategy/$winner.pat"
  echo "# Experimental hook-score full fit, winner seed $best"
  tail -n +2 "$strategy/$winner.pat" | grep -v '^#' | grep -v '^opening_'
  printf '%s\n' "$rows"
} > "$strategy/$name.pat"
echo "candidate: $strategy/$name.pat"
