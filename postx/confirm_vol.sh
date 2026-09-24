#!/bin/bash
# Larger fresh-seed confirmation of signed volatility reranks vs X.
cd "$(dirname "$0")"
for b in "hookscore_x@px_vs2@0.5" "hookscore_x@px_vs2@1" "hookscore_x@px_vs4@1"; do
  tag=confirm_${b//@/_}
  ./run_decide.sh CSW21 hookscore_x "$b" 994000000 100000 16 "$tag" 2>&1
done
