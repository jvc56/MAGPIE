#!/bin/sh
# Shippable X-recipe PAT with the utility correction for one lexicon on the
# 21x21 board:
#   test/pat_build_hookscore_super_release.sh <lexicon> <letter_distribution> \
#       <leaves> <incumbent_pat> <out_name> <log_dir> <train_seed> \
#       <validate_seed>
# The 21x21 counterpart of test/pat_build_hookscore_release.sh: one training
# seed of the hook-score v5 recipe, the lexicon's own opening table,
# utility_adjust,350 (read off the distribution's winpct_<ld>_super table),
# then a 500K-pair validation against the incumbent. Runs the 21x21 builds
# in bin_b21 (make magpie / magpie_test BUILD=no_pgo_release BOARD_DIM=21,
# copied there). No WMP or RIT: the super distributions don't fit a WMP.
set -eu
lex="$1"; ld="$2"; leaves="$3"; incumbent="$4"; name="$5"; log_dir="$6"
train_seed="$7"; validate_seed="$8"
strategy=data/strategy
threads=10
utility=350
racks=300
validate_pairs=500000
board="-ld $ld -bdn standard21 -leaves $leaves -wmp false"
mkdir -p "$log_dir"
mg() { rm -f settings.txt; ./bin_b21/magpie "$@"; }
mgt() { rm -f settings.txt; ./bin_b21/magpie_test "$@"; }
log() { echo "[$(date +%H:%M:%S)] $lex: $*"; }

awk '{ print; if ($0 == "fit_scaled,0") { print "fit_residual,5"; print "exact_created_hooks,1" } }' \
  "$strategy/pat_zero_lexsigned_nofit.pat" > "$strategy/${name}_bootstrap.pat"
log "iterative training"
mg patgen 30000,30000,30000,30000,30000 "${name}_v3" -lex "$lex" $board \
  -gp true -threads $threads -seed "$train_seed" -pat "${name}_bootstrap" \
  > "$log_dir/train.txt" 2>&1
sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,5$/fit_residual,3/' \
  "$strategy/${name}_v3.pat" > "$strategy/${name}_v3_runres.pat"
log "through refit"
mg patgen 150000 "${name}_v4" -lex "$lex" $board -gp true -threads $threads \
  -seed 4242 -pat "${name}_v3_runres" > "$log_dir/refit.txt" 2>&1
sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/${name}_v4.pat"

log "opening simulation ($racks racks)"
mgt "patopeningsim:$lex:${name}_v4:$racks:$leaves:$ld" > "$log_dir/opening_sim.txt" 2>&1
rows="$(grep -E '^opening_(tiles_[0-9]+|exchange),-?[0-9]+$' "$log_dir/opening_sim.txt")"
[ -n "$rows" ]
{
  head -1 "$strategy/${name}_v4.pat"
  echo "# $name: hook-score v5 recipe (one seed, $train_seed) for $lex on the"
  echo "# 21x21 board ($ld, leaves $leaves), its own opening table ($racks"
  echo "# racks) and utility_adjust,$utility."
  tail -n +2 "$strategy/${name}_v4.pat" | grep -v '^#' | grep -v '^opening_'
  printf '%s\n' "$rows"
  echo "utility_adjust,$utility"
} > "$strategy/$name.pat"
log "candidate: $strategy/$name.pat"

log "validation vs $incumbent ($validate_pairs pairs)"
mg autoplay games $validate_pairs -lex "$lex" $board -gp true -threads $threads \
  -seed "$validate_seed" -pat "$incumbent" -pat1 "$name" -pat2 "$incumbent" \
  > "$log_dir/validate.txt" 2>&1
log "validation: $(grep -m1 'mirrored pair' "$log_dir/validate.txt")"
rm -f "$strategy/${name}_bootstrap.pat" "$strategy/${name}_v3_runres.pat"
