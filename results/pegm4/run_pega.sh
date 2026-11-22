#!/bin/sh
# PEG (a): greedy seed only, stock vs staticbest and stock vs own (N=12).
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
run() { # bag mode oracle
  out=$R/pega_b$1_$2.txt
  if [ "$2" = "own" ]; then
    MAGPIE_EGPICK_N=12 MAGPIE_EGPICK_MODE=own MAGPIE_EGPICK_WEIGHTS=scripts/egregret/weights/cheap_own_leave.tsv $R/mt_peg "pegswitch:$R/pegclose_$1.txt,0,300,0,$3,8" > $out 2> /dev/null
  else
    MAGPIE_EGPICK_N=1 MAGPIE_EGPICK_MODE=staticbest $R/mt_peg "pegswitch:$R/pegclose_$1.txt,0,300,0,$3,8" > $out 2> /dev/null
  fi
  echo "$(date) done $out" >> $R/peg_log.txt
}
for b in 1 2; do run $b staticbest 60; run $b own 60; done
for b in 3 4; do run $b staticbest 90; run $b own 90; done
echo "$(date) pega finished" >> $R/peg_log.txt
