#!/bin/sh
# Eight-arm PEG comparison, 2-ply steps under a time budget (cut off = fall back to the step before).
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
run() { # bag max stage_tlim
  $R/mt_mid2 "pegmid:$R/pegclose_$1.txt,0,$2,600,8,$3" > $R/mid_b$1.txt 2> /dev/null
  echo "$(date) done mid_b$1.txt (stage tlim $3)" >> $R/peg_log.txt
}
run 2 150 20
run 3 100 20
echo "$(date) mid2 finished" >> $R/peg_log.txt
