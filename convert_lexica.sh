#!/bin/bash

make clean; make magpie BUILD=release BOARD_DIM=15
for path in data/lexica/*.kwg; do
    datadir=$(dirname $path)
    lexicon=$(basename $path .kwg)
    echo "Converting $lexicon.kwg to $lexicon.txt"
    bin/magpie convert dawg2text "$lexicon" "$lexicon" -lex "$lexicon"
    mv "$datadir"/"$lexicon".txt "$datadir"/"$lexicon".txt.all
    awk 'length($1) <= 15' "$datadir"/"$lexicon".txt.all > "$datadir"/"$lexicon".txt
    rm "$datadir"/"$lexicon".txt.all
done

for path in data/lexica/*.txt; do
    datadir=$(dirname $path)
    lexicon=$(basename $path .txt)
    # Skip OSPS49 because Polish language has too many letters for WMP support
    if [[ "$lexicon" = "OSPS"* ]]; then
        echo "Skipping $lexicon.txt, Polish is not supported for WMP"
        continue
    fi
    echo "Converting $lexicon.txt to $lexicon.wmp"
    bin/magpie convert text2wordmap "$lexicon" "$lexicon" -lex "$lexicon"
done