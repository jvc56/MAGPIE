#!/bin/bash
# Run one arm of the sigmoid spread-utility sweep: control (P1, uspread=0)
# vs treatment (P2, given uspread/uspreadscale), head-to-head game pairs.
# Split into chunks with distinct seeds so batch-means stats are possible
# from the aggregate summaries alone.
#
# Usage: sigmoid_sweep_arm.sh <arm_name> <uspread2> <uspreadscale2> \
#          <pairs_per_chunk> <num_chunks> <threads> <logdir> [extra args...]
# Extra args are appended to the autoplay command (e.g. -pl1 4 -pl2 4
# -tl1 10 -tl2 10 to override the default 2-ply iteration-capped budget).
set -euo pipefail

ARM_NAME=$1
USPREAD2=$2
USCALE2=$3
PAIRS_PER_CHUNK=$4
NUM_CHUNKS=$5
THREADS=$6
LOGDIR=$7
shift 7

BIN="$(dirname "$0")/../bin/magpie"
mkdir -p "$LOGDIR"

for ((chunk = 0; chunk < NUM_CHUNKS; chunk++)); do
  seed=$((42 + chunk))
  log="$LOGDIR/${ARM_NAME}_seed${seed}.log"
  if [[ -s $log ]] && grep -q "All Games" "$log"; then
    echo "skip existing $log"
    continue
  fi
  echo "=== arm=$ARM_NAME chunk=$chunk seed=$seed start $(date '+%F %T') ==="
  "$BIN" autoplay games "$PAIRS_PER_CHUNK" \
    -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all \
    -pl1 2 -pl2 2 -np1 15 -np2 15 -i1 600 -i2 600 \
    -uwin1 1 -uspread1 0 -uspreadscale1 100 \
    -uwin2 1 -uspread2 "$USPREAD2" -uspreadscale2 "$USCALE2" \
    -gp true -threads "$THREADS" -seed "$seed" "$@" >"$log" 2>&1
  echo "=== arm=$ARM_NAME chunk=$chunk seed=$seed end   $(date '+%F %T') ==="
done
