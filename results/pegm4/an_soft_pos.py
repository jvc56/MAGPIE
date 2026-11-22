# usage: python3 an_soft_pos.py <pegsoft output> [plies]
# Per-position spread of the sign errors, and where they sit by stage value.
import sys, collections
D = collections.defaultdict(dict)
for l in open(sys.argv[1]):
    if not l.startswith("PS ") or " | " not in l: continue
    head, move, rows = l.rstrip("\n").split(" | ")
    _, idx, run, tp, fid = head.split()
    if run != "X" and len(sys.argv) > 2 and fid != sys.argv[2]: continue
    d = {}
    for r in rows.split():
        a, b, w, v = r.rsplit(":", 3); d[(a, b)] = (int(w), int(v))
    D[(int(idx), run)][move] = d
sgn = lambda v: (v > 0) - (v < 0)
opt = collections.Counter(); pes = collections.Counter(); tot = collections.Counter()
band = collections.Counter(); banderr = collections.Counter(); candwin = {}
for (i, run), M in D.items():
    if run != "T": continue
    for m, d in M.items():
        x = D.get((i, "X"), {}).get(m)
        if x is None: continue
        sw = se = W = 0
        for k, (w, v) in d.items():
            if k not in x: continue
            e = x[k][1]; tot[i] += 1
            b = "<0" if v < 0 else "0..14" if v <= 14 else ">=15"
            band[b] += 1
            if sgn(v) > sgn(e): opt[i] += 1; banderr[b] += 1
            elif sgn(v) < sgn(e): pes[i] += 1; banderr[b] += 1
            sw += w * (sgn(v) + 1) / 2; se += w * (sgn(e) + 1) / 2; W += w
        if W: candwin[(i, m)] = (sw / W, se / W)
n = len(tot); T = sum(tot.values())
print("positions", n, "scenarios", T)
so = sorted(opt.values(), reverse=True)
print("too-good errors:", sum(so), "in", len(so), "positions; top 3 positions hold", sum(so[:3]), "; per position:", so)
print("too-bad errors:", sum(pes.values()), "in", len(pes), "positions:", sorted(pes.values(), reverse=True))
for b in ("<0", "0..14", ">=15"):
    print(f"  stage value {b:6}: {100*band[b]/T:5.1f}% of scenarios, {banderr[b]} errors")
# position-level win lost: pick by stage sign win vs best exact
lost = []
for i in tot:
    c = [(v, m) for (pi, m), v in candwin.items() if pi == i]
    if len(c) < 2: continue
    pick = max(c, key=lambda t: t[0][0]); best = max(t[0][1] for t in c)
    lost.append(best - pick[0][1])
import math
mu = sum(lost) / len(lost); sd = (sum((a - mu) ** 2 for a in lost) / max(1, len(lost) - 1)) ** .5
print(f"win lost by the stage's pick vs best emptier: {100*mu:.2f} pp (SE {100*sd/math.sqrt(len(lost)):.2f}), n={len(lost)}, nonzero on {sum(a > 1e-9 for a in lost)}")
bias = [v[0] - v[1] for v in candwin.values()]
print(f"mean candidate win bias (stage - exact): {100*sum(bias)/len(bias):+.2f} pp over {len(bias)} candidates")
