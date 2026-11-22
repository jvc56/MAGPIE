# usage: python3 an_p1.py <egpickq equal-time outputs...>
import sys, math
def ms(x):
    n = len(x); m = sum(x) / n if n else 0.0
    return m, (math.sqrt(sum((v - m) ** 2 for v in x) / (n - 1) / n) if n > 1 else 0.0)
for path in sys.argv[1:]:
    T, rows = {}, []
    for l in open(path):
        f = l.split()
        if not f: continue
        if f[0] == "T": T[f[1]] = (int(f[4]), int(f[6]), int(f[8]), int(f[10]))  # depths: stock, switch, judge stock, judge switch
        elif f[0] == "TT" and f[1] in T:
            rows.append((T[f[1]], float(f[2]), float(f[3]), int(f[4]) - int(f[5]), int(f[6])))
    if not rows: print(path, "no rows"); continue
    n = len(rows); g, se = ms([r[3] for r in rows])
    print(f"{path.split('/')[-1]}: n={n} differ={sum(r[4] for r in rows)} static gain {g:+.3f} (SE {se:.3f})  elapsed stock {1000*ms([r[1] for r in rows])[0]:.1f} ms switch {1000*ms([r[2] for r in rows])[0]:.1f} ms")
    for name, sel in (("both depth<=1", [r for r in rows if r[0][0] <= 1 and r[0][1] <= 1]), ("otherwise", [r for r in rows if not (r[0][0] <= 1 and r[0][1] <= 1)])):
        if sel:
            g, se = ms([r[3] for r in sel]); print(f"    {name:14s} n={len(sel):5d} differ={sum(r[4] for r in sel):4d} gain {g:+.3f} (SE {se:.3f})  sum {sum(r[3] for r in sel):+d}")
    d = [r for r in rows if r[4]]
    if d:
        print(f"    mean depth stock {ms([r[0][0] for r in rows])[0]:.2f} switch {ms([r[0][1] for r in rows])[0]:.2f}; judge depth on differing: stock's move {ms([r[0][2] for r in d])[0]:.1f} switch's {ms([r[0][3] for r in d])[0]:.1f}")
