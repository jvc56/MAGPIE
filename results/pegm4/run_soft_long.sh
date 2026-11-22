#!/bin/sh
# End-of-game reference with a 25-minute limit on bag-2 positions 0-39 (the ones with stage values at 2-6 plies),
# so the stopping-rule replay includes the positions the 300 s reference could not finish.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
PEGSOFT_EXACT_OPTS="-pegplies end" $R/mt_soft3 "pegsoft:$R/pegclose_2.txt,0,40,8,60,1500" > $R/soft_end_b2_long.txt 2> /dev/null
echo "$(date) done soft_end_b2_long.txt" >> $R/peg_log.txt
