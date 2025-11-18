#!/bin/bash

# Test script for game pairs with simulated inference
# Known issue: bag overflow error - debugging in progress
# Seed 1337 chosen to trigger ZERK/ZEX rack early

export MAGPIE_LOG_ROLLOUTS=1

./bin/magpie autoplay games 4 -lex TWL98 \
  -sp2 5 -np2 30 -ip2 50000 -is2 true \
  -numleaves 0 \
  -gp true \
  -mts true \
  -seed 1337 \
  -pfrequency 0 \
  -printboard true \
  -hr true \
  -threads 20
