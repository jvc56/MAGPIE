#!/bin/sh
# Trains and validates 21x21 PAT weights for one lexicon per language (see
# test/pat_build_super.sh), one after another:
#   test/pat_build_super_all.sh <log_root>
# Each candidate is written to data/strategy/<lexicon>_super21_candidate.pat
# and validated against <lexicon>_super21.pat. The other English lexica share
# CSW24's file.
set -u
log_root="$1"
mkdir -p "$log_root"
build() { # lexicon letter_distribution
  lex="$1"; ld="$2"
  sh test/pat_build_super.sh "$lex" "$ld" "${lex}_super21" "${lex}_super21" \
    "${lex}_super21_candidate" "$log_root/$lex" 61000001 71000001 \
    >> "$log_root/build.log" 2>&1 ||
    echo "[$(date +%H:%M:%S)] $lex: FAILED" >> "$log_root/build.log"
}
build CSW24 english_super
build FRA20 french_super
build RD29 german_super
build DSW25 dutch_super
build DISC2 catalan_super
build OSPS49 polish_super
echo "[$(date +%H:%M:%S)] ALL_SUPER_BUILDS_DONE" >> "$log_root/build.log"
