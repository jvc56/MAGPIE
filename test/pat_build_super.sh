#!/bin/sh
# Trains PAT weights for one lexicon on the 21x21 board and validates them
# against its current weights; the 21x21 counterpart of test/pat_build.sh:
#   test/pat_build_super.sh <lexicon> <letter_distribution> <leaves> \
#       <incumbent_pat> <out_name> <log_dir> <train_seed> <validate_seed>
# Writes data/strategy/<out_name>.pat; promote it to <lexicon>_super21.pat
# once the validation looks right. Same steps as test/pat_build.sh, with the
# utility correction read off the distribution's winpct_<ld>_super table and
# a 300-rack opening table. Runs the 21x21 builds in bin_b21 (make magpie
# magpie_test BUILD=no_pgo_release BOARD_DIM=21, copied there). No WMP or
# RIT: the super distributions don't fit a WMP.
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

cp test/pat_bootstrap.pat "$strategy/${name}_bootstrap.pat"
log "iterative training"
mg patgen 30000,30000,30000,30000,30000 "${name}_v3" -lex "$lex" $board \
  -gp true -threads $threads -seed "$train_seed" -pat "${name}_bootstrap" \
  > "$log_dir/train.txt" 2>&1
sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,5$/fit_residual,3/' \
  "$strategy/${name}_v3.pat" > "$strategy/${name}_v3_runres.pat"
log "through refit"
mg patgen 150000 "${name}_v4" -lex "$lex" $board -gp true -threads $threads \
  -seed 4242 -pat "${name}_v3_runres" > "$log_dir/refit.txt" 2>&1
sed -i.bak 's/^fit_residual,3$/fit_residual,0/' "$strategy/${name}_v4.pat"
rm -f "$strategy/${name}_v4.pat.bak"

log "opening simulation ($racks racks)"
mgt "patopeningsim:$lex:${name}_v4:$racks:$leaves:$ld" > "$log_dir/opening_sim.txt" 2>&1
rows="$(grep -E '^opening_(tiles_[0-9]+|exchange),-?[0-9]+$' "$log_dir/opening_sim.txt")"
[ -n "$rows" ]
{
  head -1 "$strategy/${name}_v4.pat"
  echo "# $name: PAT for $lex on the 21x21 board (training seed $train_seed,"
  echo "# $ld, leaves $leaves), its own opening table ($racks racks) and"
  echo "# utility_adjust,$utility."
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
