#!/bin/sh
# After bag 2 at 10 s finishes: stop run_built.sh before its bag-3 leg, re-run bag 2 with a 60 s stage budget.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
while ! /usr/bin/grep -q "done built_b2.txt" $R/peg_log.txt; do /bin/sleep 5; done
C=$(pgrep -P 4218 mt_built)
kill 4218
[ -n "$C" ] && kill $C
echo "$(date) run_built.sh stopped before bag 3 (pids 4218 $C); bag 2 at 60 s starts" >> $R/peg_log.txt
/bin/sleep 3
$R/mt_built "pegbuilt:$R/pegclose_2.txt,0,80,600,8,60" > $R/built_b2_t60.txt 2> /dev/null
echo "$(date) done built_b2_t60.txt (stage tlim 60)" >> $R/peg_log.txt
