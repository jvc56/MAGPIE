#!/bin/bash
# Driver: runs the sigmoid spread-utility bracket + scale-check + confirm
# phases sequentially, one autoplay run at a time, analyzing and committing
# after each arm so progress is durable even if this driver is interrupted.
#
# Chunking: 20 chunks x 10 pairs = 200 pairs/arm (df=19 for the batch-means
# spread t-test, vs the original 8x25 = df=7).
set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/.." && pwd)"
LOGDIR="${SIGMOID_LOGDIR:-/private/tmp/claude-501/-Users-john-sources-may10-tui-MAGPIE/fb0f62e6-f8a7-4481-a445-a9111677e165/scratchpad/sigmoid_logs}"
REPORT="$REPO/notes/sigmoid_utility_sweep.md"
THREADS=6

cd "$REPO"

run_arm() {
  local arm=$1 uspread=$2 uscale=$3 chunks=$4 pairs=$5
  shift 5
  echo "[$(date '+%F %T')] === starting arm $arm (uspread=$uspread scale=$uscale, ${chunks}x${pairs}) ==="
  bash notes/sigmoid_sweep_arm.sh "$arm" "$uspread" "$uscale" "$pairs" "$chunks" "$THREADS" "$LOGDIR" "$@"
  echo "[$(date '+%F %T')] === arm $arm autoplay done, analyzing ==="
  local result
  result="$(python3 notes/sigmoid_sweep_analyze.py "$LOGDIR" "$arm")"
  echo "$result"
  {
    echo ""
    echo "- $(date '+%F %T') $result"
  } >>"$REPORT"
  git add "$REPORT"
  git commit -q -m "sweep: $arm arm results

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019SJ8s3CzxxYNbQpQvDAjrX" || true
  echo "[$(date '+%F %T')] === arm $arm committed ==="
}

echo "[$(date '+%F %T')] PHASE: bracket (20x10 pairs/arm, 2-ply, i=600)"
run_arm u0.10 0.10 100 20 10
run_arm u0.25 0.25 100 20 10
run_arm u0.50 0.50 100 20 10
run_arm u1.00 1.00 100 20 10

echo "[$(date '+%F %T')] PHASE: bracket complete. Marker for coordinator to pick winner + scale-check arms."
echo "BRACKET_DONE"
