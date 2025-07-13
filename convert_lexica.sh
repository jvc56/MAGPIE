#!/usr/bin/env bash

# 2) Convert all *_super.kwg files (BOARD_DIM=21)
#    note: 21x21 doesn't work with wmp
make clean
make magpie BUILD=release BOARD_DIM=21

for path in data/lexica/*_super.kwg; do
    [[ -e "$path" ]] || continue  # Skip if none found
    lexicon=$(basename "$path" .kwg)
    echo "Converting $lexicon.kwg (super) to $lexicon.txt"
    bin/magpie convert dawg2text "$lexicon"
done

# 3) Build again for nstandard (BOARD_DIM=15)
#    leaves us with a BOARD_DIM=15 binary afterward
make clean
make magpie BUILD=release BOARD_DIM=15

# 4) Convert all non-super .kwg files
for path in data/lexica/*.kwg; do
    [[ "$path" == *"_super.kwg" ]] && continue
    lexicon=$(basename "$path" .kwg)
    echo "Converting $lexicon.kwg to $lexicon.txt"
    bin/magpie convert dawg2text "$lexicon"
done

# 5) Convert all .txt (that have corresponding .kwg) to .wmp (skip OSPS)
for path in data/lexica/*.kwg; do
    lexicon=$(basename "$path" .kwg)
    if [[ "$lexicon" == OSPS* ]]; then
        echo "Skipping $lexicon.txt (Polish not supported)"
        continue
    fi
    if [[ "$lexicon" == RD* ]]; then
        echo "Skipping $lexicon.txt (German not supported)"
        continue
    fi
    if [[ "$path" == *"_super.kwg" ]]; then
        echo "Skipping $lexicon.txt (super)"
        continue
    fi
    echo "Converting $lexicon.txt to $lexicon.wmp"
    bin/magpie convert text2wordmap "$lexicon"
done
