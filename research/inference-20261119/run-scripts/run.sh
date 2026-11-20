#!/bin/bash
# Sim-likelihood replay: 150 prior leaves + the true leave per position,
# 2-ply sims, 150 rollouts per play, every 5th placement of decide-fixed100k-prod's control games.
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/simfit
for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
  INFTFIT_SEED=2126000000 INFSFIT_LEAVES=150 INFSFIT_ROLLOUTS=150 INFSFIT_PLIES=2 INFSFIT_EVERY=5 INFSFIT_THREADS=1 nice -n 5 ./bin/magpie_test_sfit infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT" > $O/out-$c.txt &
done
wait
date > $O/finished.txt
