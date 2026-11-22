# usage: python3 an_mid.py <pegmid outputs...>
import sys, math
NAMES = ["stock0", "static0", "own0", "static2", "mid", "mid2x32", "mid2x8", "static2x8"]
K = len(NAMES)
def ms(x):
    n = len(x); m = sum(x) / n
    return m, (math.sqrt(sum((v - m) ** 2 for v in x) / (n - 1) / n) if n > 1 else 0.0)
def util(w, s): return (w + 0.5 / (1 + math.exp(-s / 100.0))) / 1.5
def cmp(rows, a, b):
    dw, ds, du = [], [], []; better = worse = differ = 0
    for r in rows:
        (ma, wa, sa, _), (mb, wb, sb, _) = r[a], r[b]
        if ma == mb: dw.append(0.0); ds.append(0.0); du.append(0.0); continue
        differ += 1; dw.append(wa - wb); ds.append(sa - sb); du.append(util(wa, sa) - util(wb, sb))
        better += wa > wb; worse += wa < wb
    u, use = ms(du); w, wse = ms(dw); s, sse = ms(ds)
    return f"utility {100*u:+.2f} ({100*use:.2f}) | win {100*w:+.2f} pp ({100*wse:.2f}) {better}/{worse} | spread {s:+.2f} ({sse:.2f}) | differ {differ}"
for path in sys.argv[1:]:
    rows = []; bad = skip = 0
    for l in open(path):
        if not l.startswith("PM "): continue
        if "SKIP" in l: skip += 1; continue
        parts = l.rstrip("\n").split(" | ")
        arms = []
        for i in range(K):
            f = parts[2 + 2 * i].split()
            arms.append((parts[1 + 2 * i], float(f[0]), float(f[1]), float(f[2])))
        if any(a[1] < 0 for a in arms): bad += 1; continue
        rows.append(arms)
    if not rows: print(path, "no rows"); continue
    print(f"{path.split('/')[-1]}: n={len(rows)} unjudged={bad} skip={skip}")
    print("  mean elapsed: " + "  ".join(f"{NAMES[i]} {ms([r[i][3] for r in rows])[0]:.2f}s" for i in range(K)))
    for a in range(1, K): print(f"  {NAMES[a]:9s} vs stock0 : {cmp(rows, a, 0)}")
    for a, b in ((4, 2), (4, 1), (5, 3), (6, 3), (6, 7), (6, 5), (3, 2)): print(f"  {NAMES[a]:9s} vs {NAMES[b]:9s}: {cmp(rows, a, b)}")
