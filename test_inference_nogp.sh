#!/bin/bash

# Test script for simulated inference WITHOUT game pairs
# Player 1: Static equity
# Player 2: 2-ply sim with simulated inference
# Seed 1337 chosen to trigger ZERK/ZEX rack early

export MAGPIE_LOG_ROLLOUTS=1

./bin/magpie autoplay games 4 -lex TWL98 \
  -sp2 2 -np2 2 -ip2 100 -is2 true \
  -numleaves 0 \
  -seed 1337 \
  -printboard true \
  -threads 1
