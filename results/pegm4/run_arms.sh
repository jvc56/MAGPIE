#!/bin/sh
# Five-arm PEG comparison (stock0, static0, own0, stock2, static2), one exact oracle.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
run() { # bag max oracle
  $R/mt_arms "pegarms:$R/pegclose_$1.txt,0,$2,$3,8" > $R/arms_b$1.txt 2> /dev/null
  echo "$(date) done arms_b$1.txt" >> $R/peg_log.txt
}
run 1 300 60
run 2 300 60
run 3 150 90
echo "$(date) arms finished" >> $R/peg_log.txt
