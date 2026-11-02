#!/bin/bash
# Times the anchor-oracle replays (patches/oracle_dump_reuse.patch).
# Run from the repository root, which must hold data/.
# usage: oracle_time.sh MAGPIE_BINARY BITS_DIR [REPS]
# BITS_DIR must hold static.bits and sim.bits, recorded first with
#   ORACLE=record:BITS_DIR/static.bits MAGPIE autoplay ... (the STATIC command below)
#   ORACLE=record:BITS_DIR/sim.bits MAGPIE < notes/movegen_investigation/tools/sim_fixed.cmd
set -e
B=$1; D=$2; N=${3:-3}
CMD=notes/movegen_investigation/tools/sim_fixed.cmd
STATIC="autoplay games 3000 -lex CSW24 -wmp true -rit true -ritmmap true -wit true -s1 equity -s2 equity -r1 best -r2 best -numplays 1 -gp true -threads 1 -seed 42 -hr true"
TIMEFORMAT='%U'
for rep in $(seq $N); do
  for m in none skipall skippt skipnonpt; do
    if [ $m = none ]; then E=""; else E="$m:$D/static.bits"; fi
    t=$( { time ORACLE=$E $B $STATIC >/dev/null 2>&1 < /dev/null; } 2>&1 ); echo "static $m $t"
    if [ $m = none ]; then E=""; else E="$m:$D/sim.bits"; fi
    t=$( { time ORACLE=$E $B < $CMD >/dev/null 2>&1; } 2>&1 ); echo "sim $m $t"
  done
done | awk '{k=$1" "$2; s[k]+=$3; n[k]++} END{for(k in s) printf "%-16s mean user %.3f s\n", k, s[k]/n[k]}' | sort
