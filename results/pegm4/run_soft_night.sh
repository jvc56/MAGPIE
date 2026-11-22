#!/bin/sh
# Overnight queue behind run_soft_end.sh: more end-of-game references.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
while ! /usr/bin/grep -q "done soft_end_b2.txt" $R/peg_log.txt; do /bin/sleep 20; done
# 1. bag 2, positions 40-79 (2-ply stage values for these are also in soft_b2.txt)
PEGSOFT_EXACT_OPTS="-pegplies end" $R/mt_soft3 "pegsoft:$R/pegclose_2.txt,40,40,8,60,300" > $R/soft_end_b2_40.txt 2> /dev/null
echo "$(date) done soft_end_b2_40.txt" >> $R/peg_log.txt
# 2. bag 1, positions 0-79, stage values at 2 and 3 plies
PEGSOFT_TOPK=16,16 PEGSOFT_EXACT_OPTS="-pegplies end" $R/mt_soft3 "pegsoft:$R/pegclose_1.txt,0,80,8,60,300" > $R/soft_end_b1.txt 2> /dev/null
echo "$(date) done soft_end_b1.txt" >> $R/peg_log.txt
# 3. bag 2, positions 80-139, stage values at 2 and 3 plies
PEGSOFT_TOPK=16,16 PEGSOFT_EXACT_OPTS="-pegplies end" $R/mt_soft3 "pegsoft:$R/pegclose_2.txt,80,60,8,60,300" > $R/soft_end_b2_80.txt 2> /dev/null
echo "$(date) done soft_end_b2_80.txt" >> $R/peg_log.txt
