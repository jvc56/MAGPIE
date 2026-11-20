#!/bin/bash
# Engine replay of sim-steered weights: racks x rollouts configs, every 10th placement.
cd /Users/john/sources/nov15-softmax/MAGPIE
O=data/benchmarks/softmax-20261115/steer
for cfg in "150 20" "500 20" "150 100" "500 60"; do
  set -- $cfg
  INFDEC_SCHEDULE="4:12,10:6,17:5,25:4,38:3,53:2,75:1.5" INFTFIT_SEED=2128000000 INFTFIT_STEER=1 INFTFIT_THREADS=18 INFTFIT_EVERY=10 INFDEC_STEER_RACKS=$1 INFDEC_STEER_ROLLOUTS=$2 nice -n 5 ./bin/magpie_test_steer inftfit < $O/in-all.txt 2>/dev/null | grep "^TSTEER" > $O/out-$1-$2.txt
  date > $O/done-$1-$2.txt
done
