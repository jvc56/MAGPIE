#!/bin/bash
# Phase 2+3: scale-check the bracket winner (uspread=0.50), then a
# realistic-settings confirm run. Appends to the same driver log the
# bracket phase used so the existing tail -f monitor keeps working.
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

echo "[$(date '+%F %T')] PHASE: scale check (uspread=0.50 fixed; scale in {50,200}; scale=100 already covered by bracket's u0.50)"
run_arm u0.50_s50 0.50 50 20 10
run_arm u0.50_s200 0.50 200 20 10
echo "[$(date '+%F %T')] PHASE: scale check complete."
echo "SCALECHECK_DONE"

echo "[$(date '+%F %T')] PHASE: confirm at realistic settings (4-ply, i=1000, fewer pairs)"
run_arm confirm_u0.50 0.50 100 10 10 -pl1 4 -pl2 4 -np1 15 -np2 15 -i1 1000 -i2 1000
echo "[$(date '+%F %T')] PHASE: confirm complete."
echo "CONFIRM_DONE"
echo "SWEEP_ALL_DONE"
