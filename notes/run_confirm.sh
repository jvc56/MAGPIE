#!/bin/bash
# Phase 3: confirm run at realistic settings (4-ply, 1000 iterations),
# uspread=0.50/scale=100 vs control. Appends to the same driver log the
# earlier phases used so the existing tail -f monitor keeps working.
# Uses SWEEP_PLIES/SWEEP_NP/SWEEP_ITERS env overrides (not duplicate CLI
# flags -- see notes/sigmoid_sweep_arm.sh's header comment for why).
set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/.." && pwd)"
LOGDIR="${SIGMOID_LOGDIR:-/private/tmp/claude-501/-Users-john-sources-may10-tui-MAGPIE/fb0f62e6-f8a7-4481-a445-a9111677e165/scratchpad/sigmoid_logs}"
REPORT="$REPO/notes/sigmoid_utility_sweep.md"
THREADS=6

cd "$REPO"

run_arm() {
  local arm=$1 uspread=$2 uscale=$3 chunks=$4 pairs=$5
  echo "[$(date '+%F %T')] === starting arm $arm (uspread=$uspread scale=$uscale, ${chunks}x${pairs}) ==="
  if ! bash notes/sigmoid_sweep_arm.sh "$arm" "$uspread" "$uscale" "$pairs" "$chunks" "$THREADS" "$LOGDIR"; then
    echo "[$(date '+%F %T')] === arm $arm FAILED (see log above) -- NOT committing, aborting ===" >&2
    exit 1
  fi
  echo "[$(date '+%F %T')] === arm $arm autoplay done, analyzing ==="
  local result
  if ! result="$(python3 notes/sigmoid_sweep_analyze.py "$LOGDIR" "$arm")"; then
    echo "[$(date '+%F %T')] === arm $arm analysis FAILED -- NOT committing, aborting ===" >&2
    exit 1
  fi
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

echo "[$(date '+%F %T')] PHASE: confirm at realistic settings (4-ply, i=1000, 100 pairs)"
SWEEP_PLIES=4 SWEEP_NP=15 SWEEP_ITERS=1000 run_arm confirm_u0.50 0.50 100 10 10
echo "[$(date '+%F %T')] PHASE: confirm complete."
echo "CONFIRM_DONE"
echo "SWEEP_ALL_DONE"
