#!/bin/sh
# Shippable X-recipe PAT with the utility correction for one lexicon:
#   test/pat_build_hookscore_release.sh <lexicon> <leaves> <incumbent_pat> \
#       <out_name> <log_dir> <train_seed> <validate_seed>
# One training seed of the hook-score v5 recipe (test/pat_build_hookscore.sh,
# no seed tournament), the lexicon's own opening table, utility_adjust,350,
# then a 500K-pair validation against the lexicon's current release.
# Builds the lexicon's WMP when missing, and its RIT when missing (deleted
# again at the end: a RIT is about 1.9 GB). WIT is skipped. Tables only
# change speed, never results.
set -eu
lex="$1"; leaves="$2"; incumbent="$3"; name="$4"; log_dir="$5"
train_seed="$6"; validate_seed="$7"
strategy=data/strategy
lexica=data/lexica
threads=10
utility=350
validate_pairs=500000
mkdir -p "$log_dir"
mg() { rm -f settings.txt; ./bin/magpie "$@"; }
mgt() { rm -f settings.txt; ./bin/magpie_test "$@"; }
log() { echo "[$(date +%H:%M:%S)] $lex: $*"; }

if [ ! -f "$lexica/$lex.wmp" ]; then
  log "building $lex.wmp"
  printf 'convert text2wordmap %s\n' "$lex" | mg set -lex "$lex" -wmp false \
    > "$log_dir/build_wmp.txt" 2>&1 || true
fi
built_rit=0
if [ -f "$lexica/$lex.wmp" ] && [ ! -f "$lexica/$lex.rit" ]; then
  log "building $lex.rit"
  printf 'convert klvwmp2rit %s\n' "$lex" | mg set -lex "$lex" -leaves "$leaves" \
    -wmp true -rit false > "$log_dir/build_rit.txt" 2>&1 || true
  [ -f "$lexica/$lex.rit" ] && built_rit=1
fi
cleanup_rit() {
  if [ "$built_rit" -eq 1 ]; then rm -f "$lexica/$lex.rit"; fi
}
trap cleanup_rit EXIT
if [ -f "$lexica/$lex.wmp" ]; then tables="-wmp true"; racks=1000
else tables="-wmp false"; racks=300; fi
[ -f "$lexica/$lex.rit" ] && tables="$tables -rit true -ritmmap true"
log "tables: $tables"

awk '{ print; if ($0 == "fit_scaled,0") { print "fit_residual,5"; print "exact_created_hooks,1" } }' \
  "$strategy/pat_zero_lexsigned_nofit.pat" > "$strategy/${name}_bootstrap.pat"
log "iterative training"
mg patgen 30000,30000,30000,30000,30000 "${name}_v3" -lex "$lex" -leaves "$leaves" \
  -gp true -threads $threads -seed "$train_seed" $tables -pat "${name}_bootstrap" \
  > "$log_dir/train.txt" 2>&1
sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,5$/fit_residual,3/' \
  "$strategy/${name}_v3.pat" > "$strategy/${name}_v3_runres.pat"
log "through refit"
mg patgen 150000 "${name}_v4" -lex "$lex" -leaves "$leaves" -gp true -threads $threads \
  -seed 4242 $tables -pat "${name}_v3_runres" > "$log_dir/refit.txt" 2>&1
sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/${name}_v4.pat"

log "opening simulation ($racks racks)"
mgt "patopeningsim:$lex:${name}_v4:$racks:$leaves" > "$log_dir/opening_sim.txt" 2>&1
rows="$(grep -E '^opening_(tiles_[0-9]+|exchange),-?[0-9]+$' "$log_dir/opening_sim.txt")"
[ -n "$rows" ]
{
  head -1 "$strategy/${name}_v4.pat"
  echo "# $name: hook-score v5 recipe (one seed, $train_seed) for $lex, leaves $leaves,"
  echo "# its own opening table ($racks racks) and utility_adjust,$utility."
  tail -n +2 "$strategy/${name}_v4.pat" | grep -v '^#' | grep -v '^opening_'
  printf '%s\n' "$rows"
  echo "utility_adjust,$utility"
} > "$strategy/$name.pat"
log "candidate: $strategy/$name.pat"

log "validation vs $incumbent ($validate_pairs pairs)"
mg autoplay games $validate_pairs -lex "$lex" -leaves "$leaves" -gp true -threads $threads \
  -seed "$validate_seed" $tables -winpct winpct -pat "$incumbent" -pat1 "$name" -pat2 "$incumbent" \
  > "$log_dir/validate.txt" 2>&1
log "validation: $(grep -m1 'mirrored pair' "$log_dir/validate.txt")"
rm -f "$strategy/${name}_bootstrap.pat" "$strategy/${name}_v3_runres.pat"
