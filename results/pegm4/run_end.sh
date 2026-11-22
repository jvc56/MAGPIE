#!/bin/sh
# Item 4: bag-emptying picks of the 2-ply-vs-greedy table (bag 2), re-valued by solving to the end of the game (200 s each).
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
while ! /usr/bin/grep -q "done rejudge_b2_left.txt" $R/peg_log.txt; do sleep 30; done
PEGRJ_OPTS="-pegplies end" $R/mt_fast "pegrejudge:$R/pegclose_2.txt,$R/endcheck_moves.txt,8,200" > $R/endcheck_b2.txt 2> /dev/null
echo "$(date) done endcheck_b2.txt" >> $R/peg_log.txt
