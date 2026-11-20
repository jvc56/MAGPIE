#!/bin/bash
# More 600-rack positions (100 rollouts per play): every 20th placement at offsets 5, 10, 15 -> c600/out-o<offset>-*.txt
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/simfit
for off in 10 5 15; do
  for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
    INFTFIT_SEED=2126000000 INFSFIT_LEAVES=600 INFSFIT_ROLLOUTS=100 INFSFIT_PLIES=2 INFSFIT_EVERY=20 INFSFIT_OFFSET=$off INFSFIT_SAME=0 INFSFIT_THREADS=1 nice -n 5 ./bin/magpie_test_sfit_off infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT game" > $O/c600/part-o$off-$c.txt &
  done
  wait
  for c in 0 1 2 3 4 5 6 7 8 9 10 11; do mv $O/c600/part-o$off-$c.txt $O/c600/out-o$off-$c.txt; done
  date > $O/c600/finished-o$off.txt
done
