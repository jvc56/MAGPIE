#!/bin/sh
# Nine-arm PEG comparison with the middle stage built into the solver (MAGPIE_PEGMID_N).
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
run() { # bag max stage_tlim
  $R/mt_built "pegbuilt:$R/pegclose_$1.txt,0,$2,600,8,$3" > $R/built_b$1.txt 2> /dev/null
  echo "$(date) done built_b$1.txt (stage tlim $3)" >> $R/peg_log.txt
}
run 1 150 600
run 2 150 10
run 3 100 10
echo "$(date) built finished" >> $R/peg_log.txt
