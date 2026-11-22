# usage: python3 an_built.py <pegbuilt outputs...>
import sys, math
NAMES = ["stock0", "static0", "mid", "static2x32", "mid2x32", "static2x16", "mid2x16", "static2x8", "mid2x8"]
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
    rows = []; flags = []; bad = skip = cut = 0
    for l in open(path):
        if not l.startswith(("PB ", "PN ")): continue
        if "SKIP" in l: skip += 1; continue
        parts = l.rstrip("\n").split(" | ")
        if parts[0].split()[2] == "0": cut += 1; continue  # judge cut off: seed values only
        arms = []
        for i in range(K):
            f = parts[2 + 2 * i].split()
            arms.append((parts[1 + 2 * i], float(f[0]), float(f[1]), float(f[2])))
        if any(a[1] < 0 for a in arms): bad += 1; continue
        rows.append(arms); flags.append(parts[-1].split())
    if not rows: print(path, "no rows"); continue
    n = len(rows)
    print(f"{path.split('/')[-1]}: n={n} unjudged={bad} judge cut off={cut} skip={skip}")
    for i in range(K):
        full = sum(f[i] == "1" for f in flags); part = sum(f[i] == "1p" for f in flags)
        extra = f"  2-ply complete {full} partial {part} none {n-full-part}" if i >= 3 else ""
        print(f"  {NAMES[i]:10s} {ms([r[i][3] for r in rows])[0]:6.2f}s{extra}")
    for a in range(1, K): print(f"  {NAMES[a]:10s} vs stock0    : {cmp(rows, a, 0)}")
    for a, b in ((2, 1), (4, 3), (6, 5), (8, 7), (6, 3), (8, 3), (6, 4), (8, 6)): print(f"  {NAMES[a]:10s} vs {NAMES[b]:10s}: {cmp(rows, a, b)}")
