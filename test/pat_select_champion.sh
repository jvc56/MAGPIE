#!/bin/sh
# Incumbent-preserving PAT selection for one lexicon (Astra, 2026-09-14):
#
#   test/pat_select_champion.sh <lexicon> <incumbent_pat> <release_name> [<log_dir>] [<leaves>]
#
# <leaves> defaults to <lexicon> (the usual klv2 named after the lexicon
# itself); pass it explicitly for a lexicon whose leaves live under a
# different name (e.g. OSW1_zeroed_gen_6).
#
# 1. Build the speed tables the lexicon lacks (WMP, RIT, WIT; none for
#    lexica in NO_TABLES) and run everything with what exists.
# 2. Candidates: the cross-transfer champion (CSW21's v5) if it is not
#    the incumbent; NUM_NATIVE_SEEDS from-zero native seeds of the v5
#    recipe (through refit included); and shrinkage adaptations of the
#    incumbent on this lexicon's self-play (fit_shrink, one fixed dataset).
# 3. Screen every candidate against the incumbent by paired whole-game
#    spread on one development seed.
# 4. Freeze the best challenger (a native winner first gets this
#    lexicon's opening table), confirm that exact artifact against the
#    incumbent on an untouched seed at CONFIRM_PAIRS, and ship it only if
#    the 95% lower bound clears zero; otherwise the incumbent ships.
# 5. Validate the shipped file against no PAT, write
#    data/strategy/<release_name>.pat with the provenance in its header,
#    and delete the tables (RIT, WIT, and the WMP) when the validation's
#    95% lower bound clears SHIPPABLE_MIN_LOWER_CI -- they rebuild in
#    seconds to a minute; a file that fails the bar keeps them and is
#    flagged.
# No per-lexicon knobs: the same budgets and rules for every lexicon.
set -u
if [ "$#" -lt 3 ]; then
  echo "usage: $0 <lexicon> <incumbent_pat> <release_name> [<log_dir>] [<leaves>]" >&2
  exit 2
fi
lex="$1"; incumbent="$2"; release="$3"
log_dir="${4:-/tmp/pat_select_$release}"
leaves="${5:-$lex}"
mkdir -p "$log_dir"
strategy=data/strategy
lexica=data/lexica
threads=10
NUM_NATIVE_SEEDS=3
SCREEN_PAIRS=500000
CONFIRM_PAIRS=1000000
VALIDATE_PAIRS=500000
OPENING_RACKS=1000
OPENING_RACKS_NO_WMP=300
SHIPPABLE_MIN_LOWER_CI=1.0
NO_TABLES="OSPS49"
CROSS_TRANSFER="pat_dls_champion_v5"
train_seed_base=61000002
adapt_seed=4242
# Seeds derived from the lexicon name so a rerun repeats them exactly;
# the confirmation seed is untouched by the screening.
lex_hash=$(printf '%s' "$lex" | cksum | cut -d' ' -f1)
dev_seed=$((778000000 + (lex_hash % 1000) * 10))
confirm_seed=$((dev_seed + 5))
validate_seed=$((dev_seed + 7))

log() { echo "[$(date +%H:%M:%S)] $*"; }
# magpie autosaves settings.txt in the cwd and reloads it at startup,
# lexicon and tables included; a table deleted since then would break the
# next launch, so every launch starts clean.
mg() { rm -f settings.txt; ./bin/magpie "$@"; }
mgt() { rm -f settings.txt; ./bin/magpie_test "$@"; }
line_of() { grep -m1 "mirrored pair" "$1" | sed 's/Player 1 spread per mirrored pair: //'; }
mean_of() { grep -m1 "mirrored pair" "$1" | sed 's/.*mean \([-0-9.]*\),.*/\1/'; }
lower_of() { grep -m1 "mirrored pair" "$1" | sed 's/.*95% CI \[\([-0-9.]*\),.*/\1/'; }
gt() { [ "$(echo "$1 > $2" | bc)" -eq 1 ]; }
assert_experimental_channels_zero() {
  bad="$(grep -E '^(hook_score_d[0-9]+|lm_span_d[0-9]+|lm_ext_d[0-9]+|dws_lm_span_d[0-9]+|dws_lm_ext_d[0-9]+),' "$1" | grep -v ',0$' || true)"
  if [ -n "$bad" ]; then
    echo "ERROR: $1 carries weight on experimental channels" >&2
    return 1
  fi
}
provenance="$log_dir/provenance.txt"
: > "$provenance"
note() { echo "# $*" >> "$provenance"; }

# ---- 1. tables ------------------------------------------------------------
built_wmp=0; built_rit=0; built_wit=0
tables_allowed=1
for skip in $NO_TABLES; do [ "$skip" = "$lex" ] && tables_allowed=0; done
if [ "$tables_allowed" -eq 1 ]; then
  if [ ! -f "$lexica/$lex.wmp" ]; then
    log "building $lex.wmp"
    printf 'convert text2wordmap %s\n' "$lex" | mg set -lex "$lex" -wmp false > "$log_dir/build_wmp.txt" 2>&1 || true
    [ -f "$lexica/$lex.wmp" ] && built_wmp=1
  fi
  if [ -f "$lexica/$lex.wmp" ] && [ ! -f "$lexica/$lex.rit" ]; then
    log "building $lex.rit"
    printf 'convert klvwmp2rit %s\n' "$lex" | mg set -lex "$lex" -wmp true -rit false > "$log_dir/build_rit.txt" 2>&1 || true
    [ -f "$lexica/$lex.rit" ] && built_rit=1
  fi
  if [ ! -f "$lexica/$lex.wit" ]; then
    log "building $lex.wit"
    printf 'convert kwg2wit %s\n' "$lex" | mg set -lex "$lex" -wmp false -wit false > "$log_dir/build_wit.txt" 2>&1 || true
    [ -f "$lexica/$lex.wit" ] && built_wit=1
  fi
fi
tables=""
if [ -f "$lexica/$lex.wmp" ]; then tables="-wmp true"; else tables="-wmp false"; fi
[ -f "$lexica/$lex.rit" ] && tables="$tables -rit true -ritmmap true"
[ -f "$lexica/$lex.wit" ] && tables="$tables -wit true"
log "tables: $tables (built wmp=$built_wmp rit=$built_rit wit=$built_wit)"
note "$release: PAT for $lex (leaves $leaves) by the incumbent-preserving selection"
note "(test/pat_select_champion.sh, codex/pat-setup-value, $(date +%Y-%m-%d))."
note "Incumbent: $incumbent (sha256 $(shasum -a 256 $strategy/$incumbent.pat | cut -c1-16)). Runtime tables: $tables."

# ---- 2. candidates --------------------------------------------------------
candidates=""
if [ "$CROSS_TRANSFER" != "$incumbent" ] && [ -f "$strategy/$CROSS_TRANSFER.pat" ]; then
  candidates="$candidates $CROSS_TRANSFER"
fi
i=1
while [ "$i" -le "$NUM_NATIVE_SEEDS" ]; do
  seed=$((train_seed_base + i - 1))
  v3="${release}_s${i}_v3"; v4="${release}_s${i}_v4"
  if [ ! -f "$strategy/$v4.pat" ]; then
    log "native seed $i ($seed): training $v3"
    mg patgen 30000,30000,30000,30000,30000 "$v3" -lex "$lex" -leaves "$leaves" -gp true -threads $threads \
      -seed "$seed" $tables -pat pat_zero_lexsigned_nofit > "$log_dir/train_s$i.txt" 2>&1
    if [ -f "$strategy/$v3.pat" ]; then
      sed -e 's/^run_through,0$/run_through,1/' -e 's/^fit_residual,0$/fit_residual,3/' \
        "$strategy/$v3.pat" > "$strategy/${v3}_runres.pat"
      log "native seed $i: through refit $v4"
      mg patgen 150000 "$v4" -lex "$lex" -leaves "$leaves" -gp true -threads $threads -seed $adapt_seed \
        $tables -pat "${v3}_runres" > "$log_dir/refit_s$i.txt" 2>&1
      [ -f "$strategy/$v4.pat" ] && sed -i '' 's/^fit_residual,3$/fit_residual,0/' "$strategy/$v4.pat"
    fi
  fi
  if [ -f "$strategy/$v4.pat" ] && assert_experimental_channels_zero "$strategy/$v4.pat"; then
    candidates="$candidates $v4"
  else
    log "native seed $i failed; skipped"
  fi
  i=$((i + 1))
done
boot="${release}_adapt_boot"
sed -e 's/^fit_shrink,0$/fit_shrink,1/' "$strategy/$incumbent.pat" > "$strategy/$boot.pat"
grep -q '^fit_shrink,1$' "$strategy/$boot.pat" || sed -i '' 's/^run_through,1$/run_through,1\
fit_shrink,1/' "$strategy/$boot.pat"
log "adaptation candidates from $incumbent on $lex self-play"
mg patgen 150000 "${release}_adapt" -lex "$lex" -leaves "$leaves" -gp true -threads $threads -seed $adapt_seed \
  $tables -pat "$boot" > "$log_dir/adapt.txt" 2>&1 || log "adaptation patgen failed; skipped"
for lam in 10 100 1000; do
  c="${release}_adapt_gen_1_shrink$lam"
  [ -f "$strategy/$c.pat" ] && candidates="$candidates $c"
done
if [ -f "$strategy/${release}_adapt_gen_1_report.txt" ]; then
  sed -n '/^shrink_lambda,/,/^Installed/p' "$strategy/${release}_adapt_gen_1_report.txt" | sed 's/^/#   /' >> "$provenance"
fi
log "candidates:$candidates"

# ---- 3. screening ---------------------------------------------------------
note "Screening vs the incumbent on $lex, dev seed $dev_seed, $SCREEN_PAIRS mirrored pairs:"
best=""; best_mean=0
for c in $candidates; do
  log "screen $c vs $incumbent"
  mg autoplay games $SCREEN_PAIRS -lex "$lex" -leaves "$leaves" -gp true -threads $threads -seed $dev_seed \
    $tables -pat1 "$c" -pat2 "$incumbent" > "$log_dir/screen_$c.txt" 2>&1
  l="$(line_of "$log_dir/screen_$c.txt")"
  if [ -z "$l" ]; then log "  $c: no result"; note "  $c: failed"; continue; fi
  log "  $c: $l"
  note "  $c: $l"
  m="$(mean_of "$log_dir/screen_$c.txt")"
  if gt "$m" "$best_mean"; then best="$c"; best_mean="$m"; fi
done

# ---- 4. challenger, opening table, confirmation ---------------------------
final="$incumbent"
decision="incumbent retained: no challenger beat it on the development seed"
if [ -n "$best" ]; then
  challenger="${release}_challenger"
  case "$best" in
    ${release}_s*_v4)
      racks=$OPENING_RACKS
      [ -f "$lexica/$lex.wmp" ] || racks=$OPENING_RACKS_NO_WMP
      log "opening table for native challenger $best ($racks racks)"
      mgt "patopeningsim:$lex:$best:$racks:$leaves" > "$log_dir/opening_sim.txt" 2>&1
      rows="$(grep -E '^opening_(tiles_[0-9]+|exchange),-?[0-9]+$' "$log_dir/opening_sim.txt")"
      { head -1 "$strategy/$best.pat"; tail -n +2 "$strategy/$best.pat" | grep -v '^#' | grep -v '^opening_'; echo "$rows"; } > "$strategy/$challenger.pat"
      note "Challenger: $best (leaves $leaves) plus its opening table ($racks racks): $(echo "$rows" | tr '\n' ' ')"
      ;;
    *)
      { head -1 "$strategy/$best.pat"; tail -n +2 "$strategy/$best.pat" | grep -v '^#'; } > "$strategy/$challenger.pat"
      note "Challenger: $best as is."
      ;;
  esac
  log "confirm $challenger vs $incumbent (seed $confirm_seed, $CONFIRM_PAIRS pairs)"
  mg autoplay games $CONFIRM_PAIRS -lex "$lex" -leaves "$leaves" -gp true -threads $threads -seed $confirm_seed \
    $tables -pat1 "$challenger" -pat2 "$incumbent" > "$log_dir/confirm.txt" 2>&1
  l="$(line_of "$log_dir/confirm.txt")"
  log "  confirmation: $l"
  note "Confirmation vs the incumbent, untouched seed $confirm_seed, $CONFIRM_PAIRS pairs: $l"
  if [ -n "$l" ] && gt "$(lower_of "$log_dir/confirm.txt")" 0; then
    final="$challenger"
    decision="challenger $best shipped: confirmation lower bound above zero"
  else
    decision="incumbent retained: challenger $best did not confirm"
  fi
fi
log "$decision"
note "Decision: $decision."

# ---- 5. validation, release file, tables ----------------------------------
log "validate $final vs none (seed $validate_seed)"
mg autoplay games $VALIDATE_PAIRS -lex "$lex" -leaves "$leaves" -gp true -threads $threads -seed $validate_seed \
  $tables -pat1 "$final" -pat2 none > "$log_dir/validate.txt" 2>&1
vl="$(line_of "$log_dir/validate.txt")"
log "  validation: $vl"
note "Validation vs no PAT on $lex, seed $validate_seed, $VALIDATE_PAIRS pairs: $vl"
{
  head -1 "$strategy/$final.pat"
  cat "$provenance"
  if [ "$final" = "$incumbent" ]; then
    echo "# Rows below are byte-identical to $incumbent; its own provenance:"
    grep '^#' "$strategy/$incumbent.pat" | sed 's/^#/# |/'
  fi
  tail -n +2 "$strategy/$final.pat" | grep -v '^#'
} > "$strategy/$release.pat"
log "wrote $strategy/$release.pat"
shippable=0
if [ -n "$vl" ] && gt "$(lower_of "$log_dir/validate.txt")" "$SHIPPABLE_MIN_LOWER_CI"; then shippable=1; fi
if [ "$shippable" -eq 1 ]; then
  log "shippable (validation lower bound above $SHIPPABLE_MIN_LOWER_CI); removing tables"
  [ -f "$lexica/$lex.rit" ] && rm -f "$lexica/$lex.rit" && log "  deleted $lex.rit"
  [ -f "$lexica/$lex.wit" ] && rm -f "$lexica/$lex.wit" && log "  deleted $lex.wit"
  [ -f "$lexica/$lex.wmp" ] && rm -f "$lexica/$lex.wmp" && log "  deleted $lex.wmp"
  rm -f settings.txt
else
  log "NOT shippable by the validation bar; tables kept, file flagged"
  echo "# NOT SHIPPABLE: validation lower bound did not clear $SHIPPABLE_MIN_LOWER_CI" >> "$strategy/$release.pat.FLAG"
fi
log "done: $release ($decision)"
