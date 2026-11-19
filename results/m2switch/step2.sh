#!/bin/sh
# Timed solves that deepen 1, 2, 4, 6 (switch arm) against every depth (stock
# arm), at equal limits. Waits for the machine to be free.
W=/Users/john/sources/nov17-magpie/egswitch-work
cd /Users/john/sources/nov17-magpie/magpie-egswitch || exit 1
while pgrep -f 'magpie_test_own4|magpie_test_sw10 egpickq' > /dev/null; do sleep 15; done
export MAGPIE_EG_STEP2=arm MAGPIE_EGPICK_N=1
run() { # mode set file lex tlim max
  out=$W/step2/$1_$2_t$5.txt
  MAGPIE_EGPICK_MODE=$1 $W/magpie_test_sw10 "egpickq:file=$W/$3,lex=$4,tlim=$5,jtlim=5000,threads=4,max=$6" > $out 2> /dev/null
  echo "$(date) done $out" >> $W/step2/log.txt
}
for mode in stock staticbest; do
  run $mode nonstuck eq_nonstuck.txt CSW21 10 1000
  run $mode nonstuck eq_nonstuck.txt CSW21 50 1000
  run $mode nonstuck eq_nonstuck.txt CSW21 200 1000
  run $mode ge11 pos_ge11.txt CSW24 20 1000
  run $mode ge11 pos_ge11.txt CSW24 100 1000
  run $mode ge11 pos_ge11.txt CSW24 500 1000
done
echo "$(date) all finished" >> $W/step2/log.txt
