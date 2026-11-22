# usage: python3 an_rj.py   -- bag-2 comparisons with picks that leave tiles in the bag re-valued by the hidden-rack judge
import math
NAMES = ["stock0", "static0", "mid", "static2x32", "mid2x32"]
def util(w, s): return (w + 0.5 / (1 + math.exp(-s / 100.0))) / 1.5
def ms(x):
    n = len(x); m = sum(x) / n
    return m, (math.sqrt(sum((v - m) ** 2 for v in x) / (n - 1) / n) if n > 1 else 0.0)
rj = {}
for f in ("rejudge_b2.txt", "rejudge_b2_fast.txt"):
    for l in open(f):
        if not l.startswith("RJ ") or " | " not in l: continue
        h, m = l.rstrip("\n").split(" | "); h = h.split()
        if h[2] == "1": rj[(int(h[1]), m)] = (float(h[4]), float(h[5]))
listed = set()
for l in open("rejudge_moves.txt"):
    i, m = l.rstrip("\n").split("\t"); listed.add((int(i), m))
print("listed", len(listed), "valued", len(rj))
for path in ("built_b2.txt", "built_b2_t60.txt"):
    rows = []
    for l in open(path):
        if not l.startswith("PB ") or "SKIP" in l: continue
        p = l.rstrip("\n").split(" | ")
        idx = int(p[0].split()[1])
        if p[0].split()[2] == "0": continue
        arms = []
        for i in range(5):
            f = p[2 + 2 * i].split(); arms.append((p[1 + 2 * i], float(f[0]), float(f[1])))
        if any(a[1] < 0 for a in arms): continue
        rows.append((idx, arms))
    print(path, "rows", len(rows))
    for a, b in ((3, 0), (4, 3), (2, 1)):
        for mode in ("clairvoyant", "hidden"):
            du = []; dw = []; ds = []; differ = drop = sub = better = worse = 0
            for idx, arms in rows:
                va = arms[a]; vb = arms[b]
                if va[0] == vb[0]: du.append(0); dw.append(0); ds.append(0); continue
                vals = []; ok = True
                for v in (va, vb):
                    k = (idx, v[0])
                    if k in listed:
                        if k not in rj: ok = False
                        elif mode == "hidden": vals.append(rj[k]); sub += 1; continue
                    vals.append((v[1], v[2]))
                if not ok: drop += 1; continue
                differ += 1
                dw.append(vals[0][0] - vals[1][0]); ds.append(vals[0][1] - vals[1][1])
                du.append(util(*vals[0]) - util(*vals[1]))
                better += dw[-1] > 0; worse += dw[-1] < 0
            u, use = ms(du); w, wse = ms(dw); s, sse = ms(ds)
            print(f"  {NAMES[a]} vs {NAMES[b]} [{mode}] n={len(du)} differ {differ} dropped {drop} substituted {sub}: utility {100*u:+.2f} ({100*use:.2f}) | win {100*w:+.2f} pp ({100*wse:.2f}) {better}/{worse} | spread {s:+.2f} ({sse:.2f})")
