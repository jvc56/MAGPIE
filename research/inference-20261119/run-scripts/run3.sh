#!/bin/bash
# C: 600 sampled racks per position, 100 rollouts per play, every 20th placement -> c600/ (for the imputation curve). Starts after run2.sh.
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/simfit
until [ -f $O/b500/finished.txt ]; do sleep 30; done
mkdir -p $O/c600
for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
  INFTFIT_SEED=2126000000 INFSFIT_LEAVES=600 INFSFIT_ROLLOUTS=100 INFSFIT_PLIES=2 INFSFIT_EVERY=20 INFSFIT_SAME=0 INFSFIT_THREADS=1 nice -n 5 ./bin/magpie_test_sfit_same infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT" > $O/c600/out-$c.txt &
done
wait
date > $O/c600/finished.txt
