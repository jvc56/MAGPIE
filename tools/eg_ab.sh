#!/usr/bin/env bash
# Endgame optimization A/B validator.
# Rebuilds magpie_test (release), runs the endgame speed benchmark, and checks:
#   1. CORRECTNESS: per-position value+nodes must match the saved baseline oracle
#      (/tmp/eg_oracle.txt). Value mismatch = always a bug. Node change is only OK
#      for intentional pruning/ordering changes (flagged, not failed).
#   2. ORACLE: the known-value endgame suite (./bin/magpie_test endgame) stays green.
#   3. SPEED: total_time / total_nodes / nps vs the baseline numbers.
#
# Usage: tools/eg_ab.sh [label]   (run from repo root)
# Baseline (60 pos @ 4-ply, 1 thread): 28280896 nodes, 144.7s, ~195500 nps.
set -u
LABEL="${1:-cand}"
BASE_NODES=28280896
BASE_TIME=144.72
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

echo "=== [$LABEL] rebuild (release) ==="
if ! make BUILD=release magpie_test >/tmp/eg_build.log 2>&1; then
  echo "BUILD FAILED"; grep -iE "error:" /tmp/eg_build.log | head; exit 1
fi
grep -iE "error:|warning:" /tmp/eg_build.log | grep -v "SDK Version" | head

echo "=== [$LABEL] correctness: known-value endgame suite ==="
if ./bin/magpie_test endgame >/tmp/eg_suite.log 2>&1; then
  echo "  endgame suite: GREEN ✓"
else
  echo "  endgame suite: FAILED ✗"; grep -iE "Assert|expected" /tmp/eg_suite.log | head
fi

echo "=== [$LABEL] benchmark (60 pos @ 4-ply, 1 thread) ==="
MAGPIE_BENCH_MAX=60 MAGPIE_BENCH_PLIES=4 MAGPIE_BENCH_THREADS=1 MAGPIE_BENCH_TAG="$LABEL" \
  ./bin/magpie_test egspeedbench 2>&1 | grep BENCH > "/tmp/eg_${LABEL}.txt"
grep BENCHSUM "/tmp/eg_${LABEL}.txt"

echo "=== [$LABEL] correctness: value+nodes vs baseline oracle ==="
grep BENCHROW "/tmp/eg_${LABEL}.txt" | awk '{print $2,$3,$4}' > "/tmp/eg_${LABEL}_vn.txt"
if diff -q <(awk '{print $1,$2}' /tmp/eg_oracle.txt) <(awk '{print $1,$2}' "/tmp/eg_${LABEL}_vn.txt") >/dev/null; then
  echo "  VALUES: identical to baseline ✓"
else
  echo "  VALUES: MISMATCH ✗ (CORRECTNESS BUG)"
  diff <(awk '{print $1,$2}' /tmp/eg_oracle.txt) <(awk '{print $1,$2}' "/tmp/eg_${LABEL}_vn.txt") | head
fi
if diff -q <(awk '{print $3}' /tmp/eg_oracle.txt) <(awk '{print $3}' "/tmp/eg_${LABEL}_vn.txt") >/dev/null; then
  echo "  NODES: identical to baseline ✓ (pure refactor)"
else
  nchg=$(diff <(awk '{print $3}' /tmp/eg_oracle.txt) <(awk '{print $3}' "/tmp/eg_${LABEL}_vn.txt") | grep -c '^[<>]')
  echo "  NODES: changed on $((nchg/2)) positions (OK only if intentional pruning/ordering)"
fi

echo "=== [$LABEL] speed vs baseline ==="
NEW_TIME=$(grep BENCHSUM "/tmp/eg_${LABEL}.txt" | sed -E 's/.*total_time=([0-9.]+).*/\1/')
NEW_NODES=$(grep BENCHSUM "/tmp/eg_${LABEL}.txt" | sed -E 's/.*total_nodes=([0-9]+).*/\1/')
NEW_NPS=$(grep BENCHSUM "/tmp/eg_${LABEL}.txt" | sed -E 's/.*nps=([0-9]+).*/\1/')
awk -v bt="$BASE_TIME" -v bn="$BASE_NODES" -v nt="$NEW_TIME" -v nn="$NEW_NODES" -v np="$NEW_NPS" 'BEGIN{
  printf "  time:  baseline %.2fs -> %.2fs  (%.1f%% %s)\n", bt, nt, (bt-nt)/bt*100, (nt<bt?"faster":"slower");
  printf "  nodes: baseline %d -> %d\n", bn, nn;
  printf "  nps:   baseline ~195500 -> %d  (%.1f%% %s)\n", np, (np-195500.0)/195500*100, (np>195500?"faster":"slower");
}'
