#!/usr/bin/env bash
# One VM's share of the peg1cont experiment (see test/peg1_continuation_test.c).
#
#   tools/peg1cont/run_vm.sh VM_IDX NUM_VMS BOARDS [PROCS]
#
# Rerunning resumes: set RUN_TAG (e.g. .r1) so the new run writes
# results/shard-<k><RUN_TAG>.jsonl and skips boards already complete in that
# shard's earlier result files.
# Fetches NWL23 data if missing, builds the optimized test binary, and runs
# PROCS single-threaded processes. Process p on VM v scores the boards whose
# index % (NUM_VMS * PROCS) == v * PROCS + p. Extra PEG1C_* variables in the
# environment pass through. Every PUSH_EVERY seconds (default 600) and at the
# end, results/ is committed and pushed to $RESULTS_BRANCH (default
# claude/peg1cont-results-vm<VM_IDX>).
set -euo pipefail
VM=$1
NVM=$2
BOARDS=$3
PROCS=${4:-4}
PUSH_EVERY=${PUSH_EVERY:-600}
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

if [ ! -f data/lexica/NWL23.kwg ]; then
  TMP=$(mktemp -d)
  GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 https://github.com/jvc56/magpie-data "$TMP/magpie-data"
  cat "$TMP"/magpie-data/versioned-tarballs/data-20260925.tgz.a? | tar -xzf - -C "$ROOT"
  rm -rf "$TMP"
fi
make magpie BUILD=no_pgo_release BOARD_DIM=15 > /dev/null
if [ ! -f data/lexica/NWL23.wmp ]; then
  bin/magpie convert dawg2text NWL23 > /dev/null
  bin/magpie convert text2wordmap NWL23 -threads "$(nproc)" > /dev/null
fi
make magpie_test BUILD=no_pgo_release BOARD_DIM=15 > /dev/null

BRANCH=${RESULTS_BRANCH:-claude/peg1cont-results-vm$VM}
git config user.email > /dev/null || git config user.email noreply@anthropic.com
git config user.name > /dev/null || git config user.name "peg1cont"
push_results() {
  git add -f results/ 2>/dev/null || true
  git commit -q -m "peg1cont results vm$VM" || true
  # This VM's branch only ever gains commits, so a plain push fast-forwards.
  for delay in 2 4 8 16; do
    if git push -q origin "HEAD:refs/heads/$BRANCH"; then
      return 0
    fi
    sleep "$delay"
  done
  echo "push failed" >&2
}

mkdir -p results
NSHARDS=$((NVM * PROCS))
pids=()
RUN_TAG=${RUN_TAG:-}
for p in $(seq 0 $((PROCS - 1))); do
  SHARD=$((VM * PROCS + p))
  SKIP=$(ls results/shard-"$SHARD".jsonl results/shard-"$SHARD".*.jsonl 2> /dev/null | paste -sd, - || true)
  PEG1C_OUT="results/shard-$SHARD$RUN_TAG.jsonl" PEG1C_SKIP_FROM="$SKIP" \
    PEG1C_SHARD=$SHARD PEG1C_NSHARDS=$NSHARDS PEG1C_BOARDS=$BOARDS \
    ./bin/magpie_test peg1cont > "results/shard-$SHARD$RUN_TAG.log" 2>&1 &
  pids+=($!)
done

last_push=$SECONDS
while true; do
  running=0
  for pid in "${pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      running=1
    fi
  done
  if [ "$running" = 0 ]; then
    break
  fi
  sleep 30
  if [ $((SECONDS - last_push)) -ge "$PUSH_EVERY" ]; then
    push_results
    last_push=$SECONDS
  fi
done
status=0
for pid in "${pids[@]}"; do
  wait "$pid" || status=1
done
echo "exit $status" > "results/vm$VM$RUN_TAG.done"
push_results
exit $status
