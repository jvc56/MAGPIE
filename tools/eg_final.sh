#!/usr/bin/env bash
# Drift-immune final A/B: build three magpie_test binaries and time them
# INTERLEAVED so slow machine drift (thermal/scheduling) cancels in the median.
#   mt_base : baseline solver (move_gen.{c,h} at the harness commit f601496f)
#   mt_r2   : the 5 committed pure-refactors (HEAD, f0eb81b9)
#   mt_69   : HEAD + the uncommitted ranks 6+9 (current working tree)
# Then reports median total_time per binary over N interleaved rounds.
set -u
cd "$(cd "$(dirname "$0")/.." && pwd)" || exit 1
POS=30 ; PLIES=4 ; ROUNDS=4
BASE_COMMIT=f601496f

build() { make BUILD=release magpie_test >/tmp/eg_final_build.log 2>&1 || { echo "BUILD FAIL"; tail -3 /tmp/eg_final_build.log; exit 1; }; }

echo "### building mt_69 (HEAD + working-tree 6+9)"
build ; cp bin/magpie_test /tmp/mt_69

echo "### building mt_r2 (HEAD = 5 committed refactors)"
git stash push -q src/impl/move_gen.c
build ; cp bin/magpie_test /tmp/mt_r2

echo "### building mt_base (baseline solver at $BASE_COMMIT)"
git checkout -q "$BASE_COMMIT" -- src/impl/move_gen.c src/impl/move_gen.h
build ; cp bin/magpie_test /tmp/mt_base

echo "### restoring working tree"
git checkout -q HEAD -- src/impl/move_gen.c src/impl/move_gen.h
git stash pop -q
build ; cp bin/magpie_test /tmp/mt_69   # ensure mt_69 == current after restore

echo ""
echo "### interleaved timing: $POS pos @ ${PLIES}-ply, 1 thread, $ROUNDS rounds"
run() { MAGPIE_BENCH_MAX=$POS MAGPIE_BENCH_PLIES=$PLIES MAGPIE_BENCH_THREADS=1 "$1" egspeedbench 2>&1 \
        | grep BENCHSUM | sed -E 's/.*total_time=([0-9.]+).*nps=([0-9]+).*/\1 \2/'; }
: > /tmp/eg_final_times.txt
for r in $(seq 1 $ROUNDS); do
  for b in base r2 69; do
    read -r t nps <<< "$(run /tmp/mt_$b)"
    echo "round $r  $b  time=$t nps=$nps"
    echo "$b $t" >> /tmp/eg_final_times.txt
  done
done

echo ""
echo "### medians (drift-immune)"
python3 - <<'PY'
import statistics as st
d={}
for ln in open('/tmp/eg_final_times.txt'):
    b,t=ln.split(); d.setdefault(b,[]).append(float(t))
med={b:st.median(v) for b,v in d.items()}
base=med['base']
for b in ['base','r2','69']:
    m=med[b]
    print(f"  {b:5s} median={m:7.3f}s   vs base: {(base-m)/base*100:+5.2f}%   (runs: {['%.2f'%x for x in d[b]]})")
print(f"  6+9 vs r2: {(med['r2']-med['69'])/med['r2']*100:+.2f}%  (positive => 6+9 faster)")
PY
