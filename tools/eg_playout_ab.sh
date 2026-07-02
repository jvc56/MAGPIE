#!/usr/bin/env bash
# Time-limited full-game endgame playout A/B: baseline solver vs the optimized
# stack, interleaved so machine drift cancels. Each move is solved under a
# per-move wall-clock budget (time-limited IDS), then the best move is played,
# until the game ends. Reports median aggregates per binary.
#
# Time-limited search is NOT bit-deterministic (the EBF soft limit stops between
# depths based on measured elapsed time), so the faster engine shows up as some
# mix of "same search, less time" and "deeper search, same time". Aggregates over
# many moves/positions are the signal.
set -u
cd "$(cd "$(dirname "$0")/.." && pwd)" || exit 1
POS=${POS:-30} ; TIMEMS=${TIMEMS:-200} ; ROUNDS=${ROUNDS:-3} ; THREADS=${THREADS:-1}
BASE_COMMIT=f601496f   # harness present, solver un-optimized

build() { make BUILD=release magpie_test >/tmp/po_build.log 2>&1 || { echo "BUILD FAIL"; tail -3 /tmp/po_build.log; exit 1; }; }

echo "### building po_ours (HEAD solver)"
build ; cp bin/magpie_test /tmp/po_ours
echo "### building po_base (baseline solver at $BASE_COMMIT)"
git checkout -q "$BASE_COMMIT" -- src/impl/move_gen.c src/impl/move_gen.h
build ; cp bin/magpie_test /tmp/po_base
echo "### restoring optimized solver"
git checkout -q HEAD -- src/impl/move_gen.c src/impl/move_gen.h
build ; cp bin/magpie_test /tmp/po_ours

echo ""
echo "### interleaved playout: $POS positions, ${TIMEMS}ms/move, $THREADS thread(s), $ROUNDS rounds"
run() { MAGPIE_PO_MAX=$POS MAGPIE_PO_TIMEMS=$TIMEMS MAGPIE_PO_THREADS=$THREADS "$1" egplayout 2>&1 \
        | grep POSUM | sed -E 's/.*completed=([0-9]+).*total_nodes=([0-9]+).*total_depth=([0-9]+).*avg_depth=([0-9.]+).*total_time=([0-9.]+).*nps=([0-9]+).*/\1 \2 \3 \4 \5 \6/'; }
: > /tmp/po_times.txt
for r in $(seq 1 $ROUNDS); do
  for b in base ours; do
    read -r comp nodes depth avgd t nps <<< "$(run /tmp/po_$b)"
    echo "round $r  $b  completed=$comp nodes=$nodes avg_depth=$avgd time=${t}s nps=$nps"
    echo "$b $comp $nodes $depth $t $nps" >> /tmp/po_times.txt
  done
done

echo ""
echo "### medians"
python3 - <<'PY'
import statistics as st
cols=['completed','nodes','depthsum','time','nps']
d={}
for ln in open('/tmp/po_times.txt'):
    p=ln.split(); b=p[0]; vals=list(map(float,p[1:]))
    d.setdefault(b,[]).append(vals)
med={b:[st.median(x[i] for x in rows) for i in range(len(cols))] for b,rows in d.items()}
def row(b):
    m=med[b]; return dict(zip(cols,m))
mb,mo=row('base'),row('ours')
print(f"  base: completed={mb['completed']:.0f} nodes={mb['nodes']:.0f} depthsum={mb['depthsum']:.0f} time={mb['time']:.2f}s nps={mb['nps']:.0f}")
print(f"  ours: completed={mo['completed']:.0f} nodes={mo['nodes']:.0f} depthsum={mo['depthsum']:.0f} time={mo['time']:.2f}s nps={mo['nps']:.0f}")
print(f"  --> nps:      {(mo['nps']-mb['nps'])/mb['nps']*100:+.2f}%  (throughput at equal budget)")
print(f"  --> nodes:    {(mo['nodes']-mb['nodes'])/mb['nodes']*100:+.2f}%  (search work done in the budget)")
print(f"  --> depthsum: {(mo['depthsum']-mb['depthsum'])/mb['depthsum']*100:+.2f}%  (aggregate exact-search depth reached)")
print(f"  --> time:     {(mo['time']-mb['time'])/mb['time']*100:+.2f}%  (wall clock for the same playouts)")
PY
