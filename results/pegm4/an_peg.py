# usage: python3 an_peg.py <pegswitch outputs...>
import sys, math
def ms(x):
    n = len(x); m = sum(x) / n
    return m, (math.sqrt(sum((v - m) ** 2 for v in x) / (n - 1) / n) if n > 1 else 0.0)
def util(w, s): return (w + 0.5 / (1 + math.exp(-s / 100.0))) / 1.5
for path in sys.argv[1:]:
    dw, ds, du, es, ew = [], [], [], [], []; differ = bad = better = worse = skip = 0
    for l in open(path):
        if not l.startswith("PS "): continue
        if l.split()[2] == "SKIP": skip += 1; continue
        f = l.rsplit(" | ", 1)[1].split()
        st_s, st_w, d, ws, ww, ss, sw, ost, e1, e2 = int(f[0]), int(f[1]), int(f[2]), float(f[3]), float(f[4]), float(f[5]), float(f[6]), int(f[7]), float(f[8]), float(f[9])
        es.append(e1); ew.append(e2)
        if d and (ws < 0 or ww < 0): bad += 1; continue
        if d:
            differ += 1; dw.append(ww - ws); ds.append(sw - ss); du.append(util(ww, sw) - util(ws, ss))
            better += ww > ws; worse += ww < ws
        else: dw.append(0.0); ds.append(0.0); du.append(0.0)
    if not dw: print(path, "no rows"); continue
    n = len(dw); w, wse = ms(dw); s, sse = ms(ds); u, use = ms(du)
    print(f"{path.split('/')[-1]}: n={n} differ={differ} unjudged={bad} skip={skip} | win {100*w:+.2f} pp (SE {100*wse:.2f}) better/worse {better}/{worse} | spread {s:+.2f} (SE {sse:.2f}) | utility {100*u:+.2f} (SE {100*use:.2f}) | elapsed stock {ms(es)[0]:.2f}s switch {ms(ew)[0]:.2f}s")
