#!/bin/bash
# Volatility (bogopoint) rerank sweep vs X on the decision diagnostic.
cd "$(dirname "$0")"
for spec in px_vol_h2@1 px_vol_h2@3 px_vol_h2@10 px_vol_h4@1 px_vol_h4@3 px_vol_h4@10; do
  tag=vol_${spec//@/_s}
  ./run_decide.sh CSW21 hookscore_x "hookscore_x@$spec" 992000000 40000 16 "$tag" 2>&1
done
