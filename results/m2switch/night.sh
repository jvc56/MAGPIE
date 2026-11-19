#!/bin/sh
# Overnight: equal-time endgame and PEG comparisons, stock vs static ranking.
# Stops itself at 07:00.
W=/Users/john/sources/nov17-magpie/egswitch-work
cd /Users/john/sources/nov17-magpie/magpie-egswitch || exit 1
BIN=$W/magpie_test_switch7
export MAGPIE_EGPICK_N=1 MAGPIE_EGPICK_MODE=staticbest
( while [ "$(date +%H%M)" != "0700" ]; do sleep 20; done; pkill -f magpie_test_switch7; pkill -f night.sh ) &
eg() { # name file tlim jtlim max
  out=$W/night/eg_$1_t$3.txt
  $BIN "egpickq:file=$W/$2,lex=${LEX:-CSW21},tlim=$3,jtlim=$4,threads=4,max=$5" > $out 2> /dev/null
  echo "$(date) done $out" >> $W/night/log.txt
}
peg() { # bag first max tlim oracle
  out=$W/night/peg_$1_t$4_f$2.txt
  $BIN "pegswitch:notes/peg_positions/random_$1peg.txt,$2,$3,$4,$5,8" > $out 2> /dev/null
  echo "$(date) done $out" >> $W/night/log.txt
}
LEX=CSW21 eg nonstuck eq_nonstuck.txt 2 5000 1000
LEX=CSW21 eg nonstuck eq_nonstuck.txt 10 5000 1000
LEX=CSW21 eg nonstuck eq_nonstuck.txt 50 5000 1000
LEX=CSW21 eg nonstuck eq_nonstuck.txt 200 5000 1000
LEX=CSW24 eg ge11 pos_ge11.txt 20 5000 1000
LEX=CSW24 eg ge11 pos_ge11.txt 100 5000 1000
LEX=CSW24 eg ge11 pos_ge11.txt 500 5000 1000
LEX=CSW21 eg hard eq_stuck_hard.txt 10 10000 500
LEX=CSW21 eg hard eq_stuck_hard.txt 100 10000 500
LEX=CSW21 eg hard eq_stuck_hard.txt 1000 10000 500
LEX=CSW21 eg partial eq_stuck_partial.txt 10 10000 200
LEX=CSW21 eg stuck eq_stuck_100pct.txt 10 10000 400
peg 1 0 100 0.3 60
peg 2 0 25 2 60
peg 1 0 100 2 60
peg 2 0 25 10 60
peg 3 0 25 10 90
peg 4 0 25 10 90
peg 3 0 25 30 120
peg 4 0 25 30 120
echo "$(date) all finished" >> $W/night/log.txt
