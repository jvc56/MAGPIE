#!/bin/sh
# Reference solved to the end of the game (-pegplies end) for positions 2..39; 0 and 1 are in soft_end_smoke.txt.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
PEGSOFT_EXACT_OPTS="-pegplies end" $R/mt_soft3 "pegsoft:$R/pegclose_2.txt,2,38,8,60,300" > $R/soft_end_b2.txt 2> /dev/null
echo "$(date) done soft_end_b2.txt" >> $R/peg_log.txt
