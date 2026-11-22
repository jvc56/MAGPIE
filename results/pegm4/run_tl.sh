#!/bin/sh
# What the order of tied candidates costs: 5 s stock solve, tied bag-emptying candidates solved to the end.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
$R/mt_tl "pegtieloss:$R/pegclose_1.txt,0,150,8,5,300" > $R/tl_b1_t5.txt 2> /dev/null
echo "$(date) done tl_b1_t5.txt" >> $R/peg_log.txt
$R/mt_tl "pegtieloss:$R/pegclose_2.txt,0,140,8,5,300" > $R/tl_b2_t5.txt 2> /dev/null
echo "$(date) done tl_b2_t5.txt" >> $R/peg_log.txt
$R/mt_tl "pegtieloss:$R/pegclose_2.txt,0,140,8,20,300" > $R/tl_b2_t20.txt 2> /dev/null
echo "$(date) done tl_b2_t20.txt" >> $R/peg_log.txt
