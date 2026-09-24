#!/bin/bash
# Signed (unclipped) vs clipped: X refits and volatility reranks vs X.
cd "$(dirname "$0")"
for b in px_xc px_xs "hookscore_x@px_vs2@1" "hookscore_x@px_vs2@3" "hookscore_x@px_vc2@1" "hookscore_x@px_vc2@3"; do
  tag=signed_${b//@/_}
  ./run_decide.sh CSW21 hookscore_x "$b" 993000000 40000 16 "$tag" 2>&1
done
