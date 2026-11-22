#!/bin/sh
# Per-scenario dump at 5 and 6 plies (first 40 positions), same settings as the 3 and 4 ply runs.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
PEGSOFT_TOPK=16,16,16,16 $R/mt_soft2 "pegsoft:$R/pegclose_2.txt,0,40,8,60,300" > $R/soft_b2_d5.txt 2> /dev/null
echo "$(date) done soft_b2_d5.txt" >> $R/peg_log.txt
PEGSOFT_TOPK=16,16,16,16,16 $R/mt_soft2 "pegsoft:$R/pegclose_2.txt,0,40,8,60,300" > $R/soft_b2_d6.txt 2> /dev/null
echo "$(date) done soft_b2_d6.txt" >> $R/peg_log.txt
