#!/bin/sh
# How timed stock solves end: stage reached, spreads known, candidates tied at the top.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
$R/mt_tie "pegtie:$R/pegclose_2.txt,0,140,8,5" > $R/tie_b2_t5.txt 2> /dev/null
echo "$(date) done tie_b2_t5.txt" >> $R/peg_log.txt
$R/mt_tie "pegtie:$R/pegclose_1.txt,0,150,8,5" > $R/tie_b1_t5.txt 2> /dev/null
echo "$(date) done tie_b1_t5.txt" >> $R/peg_log.txt
$R/mt_tie "pegtie:$R/pegclose_2.txt,0,140,8,20" > $R/tie_b2_t20.txt 2> /dev/null
echo "$(date) done tie_b2_t20.txt" >> $R/peg_log.txt
