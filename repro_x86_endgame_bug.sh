#!/bin/bash
# Reproduction script for PR #512 negamax_depth corruption bug
# https://github.com/jvc56/MAGPIE/pull/512#issuecomment-4230988203
#
# Bug: Intermittent negamax_depth corruption in multi_pvs[0] with
#      multi-threaded release builds. Reported on Apple Silicon (macOS,
#      Apple clang, -O3 -flto). This script tests whether it reproduces
#      on the current platform.
#
# Prerequisites:
#   - release binary: make magpie BUILD=release
#   - data files: ./download_data.sh && ./convert_lexica.sh
#
# Usage:
#   ./repro_x86_endgame_bug.sh [num_runs] [threads]
#   Default: 50 runs, 4 threads

set -euo pipefail

RUNS=${1:-50}
THREADS=${2:-4}
BINARY=./bin/magpie
CORRUPT=0
CORRECT=0
ERRORS=0

echo "=== negamax_depth corruption repro test ==="
echo "Platform: $(uname -m) / $(uname -s)"
echo "Compiler: $(gcc --version 2>&1 | head -1)"
echo "Binary: $BINARY"
echo "Runs: $RUNS, Threads: $THREADS"
echo ""

for i in $(seq 1 "$RUNS"); do
    OUTPUT=$(echo "set -s1 score -s2 score -eplies 7 -etopk 5 -threads $THREADS -mode sync -savesettings false
cgp GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21
endgame
shendgame 1" | timeout 120 "$BINARY" 2>&1)

    PV_LINE=$(echo "$OUTPUT" | grep '^PV 1' || true)
    DEPTH=$(echo "$PV_LINE" | grep -oP 'depth: \K[0-9]+' || true)

    if [ -z "$DEPTH" ]; then
        echo "Run $i: ERROR - no PV 1 line found"
        ERRORS=$((ERRORS + 1))
    elif [ "$DEPTH" -gt 100 ]; then
        echo "Run $i: CORRUPT | $PV_LINE"
        CORRUPT=$((CORRUPT + 1))
    else
        echo "Run $i: OK | $PV_LINE"
        CORRECT=$((CORRECT + 1))
    fi
done

echo ""
echo "=== Results ==="
echo "Correct: $CORRECT / $RUNS"
echo "Corrupt: $CORRUPT / $RUNS"
echo "Errors:  $ERRORS / $RUNS"

if [ "$CORRUPT" -gt 0 ]; then
    echo "VERDICT: BUG REPRODUCES on this platform"
    exit 1
else
    echo "VERDICT: Bug did NOT reproduce in $RUNS runs"
    exit 0
fi
