#!/bin/bash
# After the first extra 600-rack batch: 2,100 racks per position (100 rollouts per play) on every 40th placement -> d2100/,
# then the remaining 600-rack batches (offsets 5, 15).
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/simfit
until [ -f $O/c600/finished-o10.txt ]; do sleep 5; done
pkill -f run4.sh; pkill -f magpie_test_sfit_off; sleep 2; rm -f $O/c600/part-*.txt
mkdir -p $O/d2100
for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
  INFTFIT_SEED=2126000000 INFSFIT_LEAVES=2100 INFSFIT_ROLLOUTS=100 INFSFIT_PLIES=2 INFSFIT_EVERY=40 INFSFIT_OFFSET=0 INFSFIT_THREADS=1 nice -n 5 ./bin/magpie_test_sfit_off infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT game" > $O/d2100/out-$c.txt &
done
wait
date > $O/d2100/finished.txt
for off in 5 15; do
  for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
    INFTFIT_SEED=2126000000 INFSFIT_LEAVES=600 INFSFIT_ROLLOUTS=100 INFSFIT_PLIES=2 INFSFIT_EVERY=20 INFSFIT_OFFSET=$off INFSFIT_THREADS=1 nice -n 5 ./bin/magpie_test_sfit_off infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT game" > $O/c600/part-o$off-$c.txt &
  done
  wait
  for c in 0 1 2 3 4 5 6 7 8 9 10 11; do mv $O/c600/part-o$off-$c.txt $O/c600/out-o$off-$c.txt; done
  date > $O/c600/finished-o$off.txt
done
