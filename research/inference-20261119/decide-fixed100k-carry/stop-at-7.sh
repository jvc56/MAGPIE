#!/bin/bash
D=/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/decide-fixed100k-carry
while [ "$(date +%Y%m%d%H%M)" -lt 202611200700 ]; do sleep 30; done
pkill -f "[m]agpie_test_carry_frozen infdecide"
date > $D/stopped.txt
