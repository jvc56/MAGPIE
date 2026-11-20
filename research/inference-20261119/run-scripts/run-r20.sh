#!/bin/bash
# Same positions and leaves as run.sh but only 20 rollouts per play; starts when run.sh finishes.
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/simfit
until [ -f $O/finished.txt ]; do sleep 30; done
mkdir -p $O/r20
for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
  INFTFIT_SEED=2126000000 INFSFIT_LEAVES=150 INFSFIT_ROLLOUTS=20 INFSFIT_PLIES=2 INFSFIT_EVERY=5 INFSFIT_THREADS=1 nice -n 5 ./bin/magpie_test_sfit infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT" > $O/r20/out-$c.txt &
done
wait
date > $O/r20/finished.txt
