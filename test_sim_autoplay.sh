#!/bin/bash

# Test script for per-player simmed autoplay
# Usage: ./test_sim_autoplay.sh [threads] [mts]
#   threads: number of threads (default: 1)
#   mts: "true" to enable multi-threaded sims mode, "false" or omit for concurrent games mode

THREADS=${1:-1}
MTS=${2:-false}

# Build MTS flag
if [ "$MTS" = "true" ]; then
    MTS_FLAG="-mts true"
    MODE_DESC="Multi-threaded Sims Mode (1 game, sims use $THREADS threads)"
else
    MTS_FLAG=""
    MODE_DESC="Concurrent Games Mode ($THREADS games in parallel)"
fi

echo "========================================="
echo "Threading: $THREADS threads"
echo "Mode: $MODE_DESC"
echo "========================================="
echo ""

# Download data if needed
if [ ! -f "data/lexica/CSW21.kwg" ]; then
    echo "Downloading lexicon data..."
    ./download_data.sh
fi

echo "=== Test 1: Basic sim autoplay with different per-player params ==="
echo "Player 1: 2-ply sim with 5 moves"
echo "Player 2: 3-ply sim with 10 moves"
echo ""

./bin/magpie autoplay games 2 \
    -lex CSW21 \
    -sp1 2 -np1 5 -ip1 100 \
    -sp2 3 -np2 10 -ip2 200 \
    -seed 12345 \
    -threads $THREADS \
    $MTS_FLAG

echo ""
echo "=== Test 2: One player sims, one player equity ==="
echo "Player 1: 2-ply sim"
echo "Player 2: equity only (sp2=0)"
echo ""

./bin/magpie autoplay games 2 \
    -lex CSW21 \
    -sp1 2 -np1 5 \
    -sp2 0 \
    -seed 12345 \
    -threads $THREADS \
    $MTS_FLAG

echo ""
echo "========================================="
echo "All tests complete!"
echo "========================================="
