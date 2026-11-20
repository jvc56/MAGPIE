#!/bin/bash
# A: 20 rollouts per play (28 after bag-cycle rounding), same-tiles quota 3, every 5th placement -> a20/
# B: 500 rollouts per play, same-tiles quota 3, every 10th placement -> b500/
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/simfit
go() { # dir rollouts every
  mkdir -p $O/$1
  for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
    INFTFIT_SEED=2126000000 INFSFIT_LEAVES=150 INFSFIT_ROLLOUTS=$2 INFSFIT_PLIES=2 INFSFIT_EVERY=$3 INFSFIT_SAME=3 INFSFIT_THREADS=1 nice -n 5 ./bin/magpie_test_sfit_same infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT" > $O/$1/out-$c.txt &
  done
  wait
  date > $O/$1/finished.txt
}
go a20 20 5
go b500 500 10
