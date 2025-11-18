#!/bin/bash

# Test script for simulated inference WITHOUT game pairs
# Player 1: Static equity
# Player 2: 2-ply sim with simulated inference
# Seed 1337 chosen to trigger ZERK/ZEX rack early

./bin/magpie autoplay games 10 -lex CSW21 \
  -sp2 2 -np2 2 -ip2 100 -is2 true \
  -numleaves 0 \
  -seed 1337 \
  -printboard true \
  -threads 1
