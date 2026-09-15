#!/bin/sh
# A PAT for the 21x21 super board, trained from zero (the standard-board
# files do not carry the quad-square channels, and the geometry differs):
#
#   test/pat_build_super.sh <lexicon> <leaves> <release_name> [<log_dir>]
#
# Run from a repository built with BOARD_DIM=21. No speed tables (the WMP
# is board-dimension-specific). NUM_SEEDS native seeds of the v5 recipe
# (5 x 30K games, through refit); the standard-board CSW24_v5 as a
# transfer candidate; seed 1 is the reference and every other candidate
# is screened against it; the best gets this board's opening table, is
# validated against no PAT, and is written with its provenance. Budgets
# are smaller than the 15x15 pipeline's because super games are ~15x
# slower.
set -u
if [ "$#" -lt 3 ]; then
  echo "usage: $0 <lexicon> <leaves> <release_name> [<log_dir>]" >&2
  exit 2
fi
lex="$1"; leaves="$2"; release="$3"
log_dir="${4:-/tmp/pat_super_$release}"
mkdir -p "$log_dir"
strategy=data/strategy
threads=10
NUM_SEEDS=3
SCREEN_PAIRS=200000
VALIDATE_PAIRS=200000
OPENING_RACKS=300
TRANSFER="CSW24_v5"
train_seed_base=61000002
refit_seed=4242
dev_seed=779000010
validate_seed=779000017
log() { echo "[$(date +%H:%M:%S)] $*"; }
mg() { rm -f settings.txt; ./bin/magpie "$@"; }
mgt() { rm -f settings.txt; ./bin/magpie_test "$@"; }
line_of() { grep -m1 "mirrored pair" "$1" | sed 's/Player 1 spread per mirrored pair: //'; }
mean_of() { grep -m1 "mirrored pair" "$1" | sed 's/.*mean \([-0-9.]*\),.*/\1/'; }
gt() { [ "$(echo "$1 > $2" | bc)" -eq 1 ]; }
common="-lex $lex -leaves $leaves -gp true -threads $threads -wmp false"
provenance="$log_dir/provenance.txt"; : > "$provenance"
note() { echo "# $*" >> "$provenance"; }
note "$release: PAT for the 21x21 super board, $lex with leaves $leaves"
note "(test/pat_build_super.sh, codex/pat-setup-value, $(date +%Y-%m-%d)), no speed tables."

i=1
while [ "$i" -le "$NUM_SEEDS" ]; do
  seed=$((train_seed_base + i - 1)); v3="${release}_s${i}_v3"; v4="${release}_s${i}_v4"
  if [ ! -f "$strategy/$v4.pat" ]; then
    log "seed $i ($seed): training $v3"
    mg patgen 30000,30000,30000,30000,30000 "$v3" $common -seed "$seed" -pat pat_zero_lexsigned_nofit > "$log_dir/train_s$i.txt" 2>&1
    if [ -f "$strategy/$v3.pat" ]; then
      sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,0$/fit_residual,3/' "$strategy/$v3.pat" > "$strategy/${v3}_runres.pat"
      log "seed $i: through refit $v4"
      mg patgen 150000 "$v4" $common -seed $refit_seed -pat "${v3}_runres" > "$log_dir/refit_s$i.txt" 2>&1
      [ -f "$strategy/$v4.pat" ] && sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/$v4.pat"
    fi
  fi
  i=$((i + 1))
done
reference="${release}_s1_v4"
[ -f "$strategy/$reference.pat" ] || { log "seed 1 failed; abort"; exit 1; }
candidates=""
i=2
while [ "$i" -le "$NUM_SEEDS" ]; do
  [ -f "$strategy/${release}_s${i}_v4.pat" ] && candidates="$candidates ${release}_s${i}_v4"
  i=$((i + 1))
done
[ -f "$strategy/$TRANSFER.pat" ] && candidates="$candidates $TRANSFER"
note "Screening vs seed 1 ($reference) on the super board, dev seed $dev_seed, $SCREEN_PAIRS mirrored pairs:"
best="$reference"; best_mean=0
for c in $candidates; do
  log "screen $c vs $reference"
  mg autoplay games $SCREEN_PAIRS $common -seed $dev_seed -pat1 "$c" -pat2 "$reference" > "$log_dir/screen_$c.txt" 2>&1
  l="$(line_of "$log_dir/screen_$c.txt")"; log "  $c: $l"; note "  $c: $l"
  [ -n "$l" ] && gt "$(mean_of "$log_dir/screen_$c.txt")" "$best_mean" && { best="$c"; best_mean="$(mean_of "$log_dir/screen_$c.txt")"; }
done
note "Selected: $best."
log "opening table for $best ($OPENING_RACKS racks)"
mgt "patopeningsim:$lex:$best:$OPENING_RACKS" > "$log_dir/opening_sim.txt" 2>&1
rows="$(grep -E '^opening_(tiles_[0-9]+|exchange),-?[0-9]+$' "$log_dir/opening_sim.txt")"
final="${release}_final"
{ head -1 "$strategy/$best.pat"; tail -n +2 "$strategy/$best.pat" | grep -v '^#' | grep -v '^opening_'; echo "$rows"; } > "$strategy/$final.pat"
note "Opening table (patopeningsim:$lex:$best:$OPENING_RACKS): $(echo "$rows" | tr '\n' ' ')"
log "validate $final vs none"
mg autoplay games $VALIDATE_PAIRS $common -seed $validate_seed -pat1 "$final" -pat2 none > "$log_dir/validate.txt" 2>&1
vl="$(line_of "$log_dir/validate.txt")"; log "  validation: $vl"
note "Validation vs no PAT, seed $validate_seed, $VALIDATE_PAIRS pairs: $vl"
{ head -1 "$strategy/$final.pat"; cat "$provenance"; tail -n +2 "$strategy/$final.pat" | grep -v '^#'; } > "$strategy/$release.pat"
log "done: $strategy/$release.pat"
