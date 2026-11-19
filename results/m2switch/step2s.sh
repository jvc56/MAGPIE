#!/bin/sh
# Static ranking in both arms; the switch arm deepens 1, 2, 4, 6, the reference
# arm every depth, at equal time limits.
W=/Users/john/sources/nov17-magpie/egswitch-work
cd /Users/john/sources/nov17-magpie/magpie-egswitch || exit 1
export MAGPIE_EG_STEP2=arm MAGPIE_EGPICK_N=1 MAGPIE_EGPICK_MODE=staticbest MAGPIE_EGPICK_BASE=1
run() { # set file lex tlim max
  out=$W/step2s/$1_t$4.txt
  $W/magpie_test_sw11 "egpickq:file=$W/$2,lex=$3,tlim=$4,jtlim=5000,threads=4,max=$5" > $out 2> /dev/null
  echo "$(date) done $out" >> $W/step2s/log.txt
}
run ge11 pos_ge11.txt CSW24 20 3000
run ge11 pos_ge11.txt CSW24 100 3000
run nonstuck eq_nonstuck.txt CSW21 10 1000
run nonstuck eq_nonstuck.txt CSW21 50 1000
run nonstuck eq_nonstuck.txt CSW21 200 1000
run ge11 pos_ge11.txt CSW24 500 3000
echo "$(date) all finished" >> $W/step2s/log.txt
