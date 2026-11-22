#!/bin/sh
# After the 2-ply per-scenario run: the same dump at 3 and 4 plies (first 40 positions).
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
while ! /usr/bin/grep -q "done soft_b2.txt" $R/peg_log.txt; do /bin/sleep 10; done
PEGSOFT_TOPK=16,16 $R/mt_soft2 "pegsoft:$R/pegclose_2.txt,0,40,8,60,300" > $R/soft_b2_d3.txt 2> /dev/null
echo "$(date) done soft_b2_d3.txt" >> $R/peg_log.txt
PEGSOFT_TOPK=16,16,16 $R/mt_soft2 "pegsoft:$R/pegclose_2.txt,0,40,8,60,300" > $R/soft_b2_d4.txt 2> /dev/null
echo "$(date) done soft_b2_d4.txt" >> $R/peg_log.txt
