#!/bin/sh
# Eight-arm PEG comparison with an own-outplay middle stage; exact judge on the arms' moves only.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
run() { # bag max
  $R/mt_mid "pegmid:$R/pegclose_$1.txt,0,$2,600,8" > $R/mid_b$1.txt 2> /dev/null
  echo "$(date) done mid_b$1.txt" >> $R/peg_log.txt
}
run 1 300
run 2 300
run 3 150
run 4 150
echo "$(date) mid finished" >> $R/peg_log.txt
