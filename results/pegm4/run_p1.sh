#!/bin/sh
# Priority 1 for the M2 session: equal-time endgame, stock vs static ranking,
# code at origin/egregret-m2-switch de8fd49c. 4 single-thread workers.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
export MAGPIE_EGPICK_N=1 MAGPIE_EGPICK_MODE=staticbest
eg() { # name file lex tlim jtlim
  out=$R/p1_$1_t$4.txt
  $R/mt_switch "egpickq:file=results/m2switch/$2,lex=$3,tlim=$4,jtlim=$5,threads=4" > $out 2> /dev/null
  echo "$(date) done $out $(ps -axo pcpu,comm -r | sed -n 2p)" >> $R/p1_log.txt
}
echo "$(date) start" >> $R/p1_log.txt
for t in 2 10 50 200; do eg nonstuck eq_nonstuck.txt CSW21 $t 5000; done
for t in 20 100 500; do eg ge11 pos_ge11.txt CSW24 $t 5000; done
for t in 10 100 1000; do eg hard eq_stuck_hard.txt CSW21 $t 60000; done
echo "$(date) all finished" >> $R/p1_log.txt
