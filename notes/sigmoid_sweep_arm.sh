#!/bin/bash
# Run one arm of the sigmoid spread-utility sweep: control (P1, uspread=0)
# vs treatment (P2, given uspread/uspreadscale), head-to-head game pairs.
# Split into chunks with distinct seeds so batch-means stats are possible
# from the aggregate summaries alone.
#
# Usage: sigmoid_sweep_arm.sh <arm_name> <uspread2> <uspreadscale2> \
#          <pairs_per_chunk> <num_chunks> <threads> <logdir>
#
# Override the default 2-ply/600-iteration budget via env vars (do NOT
# pass -pl1/-np1/-i1/etc as extra args — magpie's arg parser rejects a
# flag given twice with a printed error but exit code 0, so a duplicate
# silently produces an empty/garbage log instead of failing loudly):
#   SWEEP_PLIES=4 SWEEP_NP=15 SWEEP_ITERS=1000 sigmoid_sweep_arm.sh ...
set -euo pipefail

ARM_NAME=$1
USPREAD2=$2
USCALE2=$3
PAIRS_PER_CHUNK=$4
NUM_CHUNKS=$5
THREADS=$6
LOGDIR=$7

PLIES="${SWEEP_PLIES:-2}"
NP="${SWEEP_NP:-15}"
ITERS="${SWEEP_ITERS:-600}"

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
    -pl1 "$PLIES" -pl2 "$PLIES" -np1 "$NP" -np2 "$NP" \
    -i1 "$ITERS" -i2 "$ITERS" \
    -uwin1 1 -uspread1 0 -uspreadscale1 100 \
    -uwin2 1 -uspread2 "$USPREAD2" -uspreadscale2 "$USCALE2" \
    -gp true -threads "$THREADS" -seed "$seed" >"$log" 2>&1
  # magpie's CLI arg errors print a message but exit 0, so `set -e` alone
  # won't catch a bad invocation — verify the expected summary landed.
  if ! grep -q "All Games" "$log"; then
    echo "FAILED: $log has no 'All Games' summary. Contents:" >&2
    cat "$log" >&2
    exit 1
  fi
  echo "=== arm=$ARM_NAME chunk=$chunk seed=$seed end   $(date '+%F %T') ==="
done
