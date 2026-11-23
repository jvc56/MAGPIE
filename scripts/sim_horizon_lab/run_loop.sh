#!/bin/sh
# LAB, NOT FOR MERGE. Runs the sim horizon lab (test/sim_horizon_test.c) in
# chunks of 25 games, from the repository's root:
#
#   scripts/sim_horizon_lab/run_loop.sh RESULTS_DIR FIRST_GAME CHUNKS
#
# Build first:  make magpie_test BUILD=no_pgo_release && cp bin/magpie_test bin/mt_horizon
#
# Each chunk writes RESULTS_DIR/v2_g<first game>.out and a line in
# RESULTS_DIR/run_log.txt. A file named STOP in RESULTS_DIR ends the loop
# after the current chunk. RESULTS_DIR/run_spec.txt, if present, replaces the
# settings below for the chunks that start after it is written.
results=$1
first=$2
chunks=$3
spec="plies=2,screen=3000,main=48000,cont=32,oracle=24000,mid=12000,poolshift=exact"
mkdir -p "$results" || exit 1
chunk=0
while [ "$chunk" -lt "$chunks" ]; do
  if [ -f "$results/STOP" ]; then
    echo "stopped by STOP file $(date '+%Y-%m-%d %H:%M')" >> "$results/run_log.txt"
    break
  fi
  if [ -f "$results/run_spec.txt" ]; then
    spec=$(cat "$results/run_spec.txt")
  fi
  game=$((first + 25 * chunk))
  ./bin/mt_horizon "simhorizon:first=$game,games=25,$spec" > "$results/v2_g$game.out" 2>&1
  echo "chunk $game exit=$? $(date '+%Y-%m-%d %H:%M') $spec" >> "$results/run_log.txt"
  chunk=$((chunk + 1))
done
echo "loop finished $(date '+%Y-%m-%d %H:%M')" >> "$results/run_log.txt"
