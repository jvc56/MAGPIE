#!/bin/sh
# Builds one 21x21 X-recipe PAT with the utility correction per language
# (see test/pat_build_hookscore_super_release.sh), one after another:
#   test/pat_build_super_all.sh [log_root]
# Other English lexica share CSW24's file, as on 15x15.
set -u
log_root="${1:-$HOME/sources/magpie-pat-postx/postx/super21_build}"
mkdir -p "$log_root"
build() { # lexicon letter_distribution
  lex="$1"; ld="$2"
  sh test/pat_build_hookscore_super_release.sh "$lex" "$ld" \
    "${lex}_super21" "${lex}_super21" "${lex}_super21_hsx_u350" \
    "$log_root/$lex" 61000001 71000001 >> "$log_root/build.log" 2>&1 ||
    echo "[$(date +%H:%M:%S)] $lex: FAILED" >> "$log_root/build.log"
}
build CSW24 english_super
build FRA20 french_super
build RD29 german_super
build DSW25 dutch_super
build DISC2 catalan_super
build OSPS49 polish_super
echo "[$(date +%H:%M:%S)] ALL_SUPER_BUILDS_DONE" >> "$log_root/build.log"
