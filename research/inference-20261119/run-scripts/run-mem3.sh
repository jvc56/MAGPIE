#!/bin/bash
# Three-turn carry-over chain, 60 racks per position, no sims -> mem3/
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/simfit
mkdir -p $O/mem3
for c in 0 1 2 3 4 5 6 7 8 9 10 11; do
  INFTFIT_SEED=2126000000 INFSFIT_LEAVES=60 INFSFIT_EVERY=5 INFSFIT_NOSIM=1 INFSFIT_MEMDEPTH=3 nice -n 5 ./bin/magpie_test_sfit_mem2 infsfit < $O/in-$c.txt 2>/dev/null | grep "^SFIT game" | sed 's/ sw=.*//' > $O/mem3/out-$c.txt &
done
wait
date > $O/mem3/finished.txt
