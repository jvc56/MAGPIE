# usage: python3 an_soft.py <pegsoft output> [plies: keep only T/F candidates scored at this depth]
# 1. first-win check: sign of the 2-ply full-window value (T) vs the first-win value (F), per scenario.
# 2. soft win: 2-ply spread vs the exact outcome (X) per scenario of bag-emptying candidates.
import sys, math, collections, random
D = collections.defaultdict(dict)  # (idx, run) -> move -> {scen: (weight, value)}
tiles = {}
for l in open(sys.argv[1]):
    if not l.startswith("PS ") or " | " not in l: continue
    head, move, rows = l.rstrip("\n").split(" | ")
    _, idx, run, tp, fid = head.split()
    if run != "X" and len(sys.argv) > 2 and fid != sys.argv[2]: continue
    d = {}
    for r in rows.split():
        a, b, w, v = r.rsplit(":", 3)
        d[(a, b)] = (int(w), int(v))
    D[(int(idx), run)][move] = d; tiles[(int(idx), move)] = int(tp)
sgn = lambda v: (v > 0) - (v < 0)
pos = sorted(set(i for i, r in D if r == "T"))
print("positions", len(pos))
# 1. first-win check (all candidates present in both)
same = diff = 0; ex = []
for i in pos:
    for m, d in D[(i, "T")].items():
        f = D.get((i, "F"), {}).get(m)
        if f is None: continue
        for k, (w, v) in d.items():
            if k not in f: continue
            if sgn(v) == sgn(f[k][1]): same += 1
            else:
                diff += 1
                if len(ex) < 8: ex.append((i, m, k, v, f[k][1]))
print(f"first-win check: scenarios same sign {same}, different {diff}")
for e in ex: print("   differs:", e)
# 2. soft win on emptiers
pts = []  # (pos, move, weight, s2, y)
for i in pos:
    for m, d in D[(i, "T")].items():
        x = D.get((i, "X"), {}).get(m)
        if x is None: continue
        for k, (w, v) in d.items():
            if k in x: pts.append((i, m, w, v, (sgn(x[k][1]) + 1) / 2, x[k][1]))
print("emptier scenarios with exact outcome:", len(pts), "candidates", len(set((p[0], p[1]) for p in pts)))
if not pts: sys.exit()
agree = sum(p[2] for p in pts if (sgn(p[3]) + 1) / 2 == p[4]) / sum(p[2] for p in pts)
print(f"stage sign = exact result on {100*agree:.1f}% of scenario weight")
cnt = collections.Counter((sgn(p[3]), sgn(p[5])) for p in pts)
opt = cnt[(1, -1)] + cnt[(1, 0)] + cnt[(0, -1)]; pes = cnt[(-1, 1)] + cnt[(-1, 0)] + cnt[(0, 1)]
print(f"scenario counts: too good for us {opt} (win->loss {cnt[(1,-1)]}, win->draw {cnt[(1,0)]}, draw->loss {cnt[(0,-1)]}); too bad for us {pes} (loss->win {cnt[(-1,1)]}, loss->draw {cnt[(-1,0)]}, draw->win {cnt[(0,1)]}); right {cnt[(1,1)]+cnt[(0,0)]+cnt[(-1,-1)]}")
print("reliability (2-ply spread bin: exact win rate, weight share):")
tw = sum(p[2] for p in pts)
for lo, hi in ((-999, -60), (-60, -30), (-30, -15), (-15, -5), (-5, 0), (0, 0.5), (0.5, 5), (5, 15), (15, 30), (30, 60), (60, 999)):
    b = [p for p in pts if lo <= p[3] < hi]
    if b:
        w = sum(p[2] for p in b); print(f"   [{lo:5},{hi:5}): {sum(p[2]*p[4] for p in b)/w:.3f}  ({100*w/tw:.1f}%)")
def sig(z): return 1 / (1 + math.exp(-max(-30.0, min(30.0, z))))
def fit(P):
    a, c = 0.0, 0.05
    for _ in range(60):
        ga = gc = 0.0; haa = hac = hcc = 1e-9
        for _, _, w, s, y, _ in P:
            p = sig(a + c * s); g = w * (p - y); h = w * p * (1 - p)
            ga += g; gc += g * s; haa += h; hac += h * s; hcc += h * s * s
        det = haa * hcc - hac * hac
        if abs(det) < 1e-12: break
        da = (hcc * ga - hac * gc) / det; dc = (haa * gc - hac * ga) / det
        a -= max(-1.0, min(1.0, da)); c -= max(-0.05, min(0.05, dc)); c = max(c, 1e-3)
    return a, c
a, c = fit(pts)
print(f"logistic fit: P(win) = sigmoid({a:+.3f} + s / {1/c:.1f})")
# candidate-level: true win vs hard-sign estimate vs soft estimate (leave-one-position-out fit)
cands = collections.defaultdict(list)
for p in pts: cands[(p[0], p[1])].append(p)
eh = es = 0.0; n = 0; rows = collections.defaultdict(list)
for i in pos:
    tr = [p for p in pts if p[0] != i]
    if not tr: continue
    a, c = fit(tr)
    for (pi, m), P in cands.items():
        if pi != i: continue
        w = sum(p[2] for p in P)
        if any(k not in D[(i, "X")][m] for k in D[(i, "T")][m]): continue  # need every scenario
        true = sum(p[2] * p[4] for p in P) / w
        hard = sum(p[2] * (sgn(p[3]) + 1) / 2 for p in P) / w
        soft = sum(p[2] * sig(a + c * p[3]) for p in P) / w
        eh += (hard - true) ** 2; es += (soft - true) ** 2; n += 1
        rows[i].append((m, true, hard, soft))
if n:
    print(f"candidate win estimate, RMS error vs exact: hard sign {math.sqrt(eh/n):.4f}, soft {math.sqrt(es/n):.4f}  (n={n}, leave-one-position-out)")
    lh = ls = 0.0; k = 0; bh = bs = 0
    for i, R in rows.items():
        if len(R) < 2: continue
        best = max(r[1] for r in R)
        ph = max(R, key=lambda r: (r[2], r[3])); ps = max(R, key=lambda r: r[3])
        lh += best - ph[1]; ls += best - ps[1]; k += 1; bh += ph[1] > ps[1]; bs += ps[1] > ph[1]
    if k: print(f"picking among judged emptiers: mean win lost vs best: hard sign {100*lh/k:.2f} pp, soft {100*ls/k:.2f} pp; soft better on {bs}, hard better on {bh} of {k} positions")
