#!/bin/sh
# Usage: sim_bench_bin.sh <magpie_binary> <out_file> <extra set options...>
# Fixed-iteration 2-ply sims of 15 candidates on 6 positions, THREADS threads
# (default 10), after one warm-up sim. Prints position, iterations, seconds.
bin="$1"; out="$2"; shift 2
extra="$*"
P=$HOME/sources/bs-data/p2-pools-CSW24/positions.csv
DP=$HOME/sources/bs-data/datapath
: > "$out"
cd $HOME/sources/magpie-bst
run() {
  CGP=$(sed -n ${1}p $P | cut -d, -f6-)
  rm -f settings.txt
  printf 'set -path %s:./data -lex CSW24 -wmp true -rit true -ritmmap true -wit true -s1 equity -s2 equity -r1 best -r2 best -threads %s -plies 2 -numplays 15 -threshold none -sr rr -minp 1 -seed 7 %s\ncgp %s\ngen\nsim\n' "$DP" "${THREADS:-10}" "$extra" "$CGP" | "$bin" 2>&1
}
run 2 > /dev/null
for line in 2 3 4 5 6 7; do
  res=$(run $line)
  it=$(echo "$res" | grep -o "Iters: *[0-9]*" | tail -1 | grep -o "[0-9]*$")
  tm=$(echo "$res" | grep -o "Time: *[0-9.]*" | tail -1 | grep -o "[0-9.]*$")
  echo "$line $it $tm" >> "$out"
done
