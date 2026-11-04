#!/usr/bin/env bash

shopt -s nullglob

# 1) Convert all *_super.kwg files (BOARD_DIM=21)
#    note: 21x21 doesn't work with wmp
#    The data set has none (its 21x21 files are *_super21.klv2 leaves, which
#    use the standard lexica), so only build for 21x21 when there are some.
super_kwgs=(data/lexica/*_super.kwg)
if (( ${#super_kwgs[@]} > 0 )); then
    make clean
    make magpie BUILD=no_pgo_release BOARD_DIM=21

    for path in "${super_kwgs[@]}"; do
        lexicon=$(basename "$path" .kwg)
        echo "Converting $lexicon.kwg (super) to $lexicon.txt"
        bin/magpie convert dawg2text "$lexicon"
    done
fi

# 2) Build for standard (BOARD_DIM=15)
#    leaves us with a BOARD_DIM=15 binary afterward
make clean
make magpie BUILD=no_pgo_release BOARD_DIM=15

# 3) Convert all non-super .kwg files
for path in data/lexica/*.kwg; do
    [[ "$path" == *"_super.kwg" ]] && continue
    lexicon=$(basename "$path" .kwg)
    echo "Converting $lexicon.kwg to $lexicon.txt"
    bin/magpie convert dawg2text "$lexicon"
done

# 4) Convert all .txt (that have corresponding .kwg) to .wmp (skip OSPS)
for path in data/lexica/*.kwg; do
    lexicon=$(basename "$path" .kwg)
    if [[ "$lexicon" == OSPS* ]]; then
        echo "Skipping $lexicon.txt (Polish not supported)"
        continue
    fi
    if [[ "$path" == *"_super.kwg" ]]; then
        echo "Skipping $lexicon.txt (super)"
        continue
    fi
    echo "Converting $lexicon.txt to $lexicon.wmp"
    bin/magpie convert text2wordmap "$lexicon" -threads 4
done
