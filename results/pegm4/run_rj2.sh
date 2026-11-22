#!/bin/sh
# After the tie-loss run: 57 bag-2 picks that leave tiles in the bag, valued by the hidden-rack win-first judge
# (3-ply endgames, 400 s each): the reference, then the fast judge on the same picks (must match; speed compared).
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
while ! /usr/bin/grep -q "done tl_b2_t20.txt" $R/peg_log.txt; do /bin/sleep 20; done
$R/mt_fast "pegrejudge:$R/pegclose_2.txt,$R/rejudge_moves.txt,8,400" > $R/rejudge_b2.txt 2> /dev/null
echo "$(date) done rejudge_b2.txt" >> $R/peg_log.txt
MAGPIE_PEG_FASTJUDGE=1 $R/mt_fast "pegrejudge:$R/pegclose_2.txt,$R/rejudge_moves.txt,8,400" > $R/rejudge_b2_fast.txt 2> /dev/null
echo "$(date) done rejudge_b2_fast.txt" >> $R/peg_log.txt
