#!/bin/bash

# Debug script for game pairs with simulated inference using lldb
# Known issue: bag overflow error - debugging in progress
# Seed 1337 chosen to trigger ZERK/ZEX rack early

lldb ./bin/magpie -- autoplay games 1 -lex TWL98 \
  -sp2 2 -np2 2 -ip2 50000 -is2 true \
  -numleaves 0 \
  -gp true \
  -mts true \

  -seed 1337 \
  -printboard true \
  -threads 1
