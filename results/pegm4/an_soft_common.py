# Sign errors by depth on the scenarios present at every depth (same position, candidate, scenario).
import collections
files = {2: "soft_b2.txt", 3: "soft_b2_d3.txt", 4: "soft_b2_d4.txt", 5: "soft_b2_d5.txt", 6: "soft_b2_d6.txt"}
sgn = lambda v: (v > 0) - (v < 0)
V = {}; X = {}
for dep, fn in files.items():
    V[dep] = {}
    for l in open(fn):
        if not l.startswith("PS ") or " | " not in l: continue
        head, move, rows = l.rstrip("\n").split(" | ")
        _, idx, run, tp, fid = head.split()
        if run == "F" or (run == "T" and fid != str(dep)): continue
        for r in rows.split():
            a, b, w, v = r.rsplit(":", 3)
            k = (int(idx), move, a, b)
            if run == "X": X[k] = int(v)
            else: V[dep][k] = int(v)
common = set(X)
for dep in files: common &= set(V[dep])
print("common scenarios", len(common), "positions", len(set(k[0] for k in common)), "candidates", len(set(k[:2] for k in common)))
for dep in files:
    o = collections.Counter(); p = 0
    for k in common:
        d = sgn(V[dep][k]) - sgn(X[k])
        if d > 0: o[k[0]] += 1
        elif d < 0: p += 1
    print(f"{dep}-ply: too good {sum(o.values())} (in {len(o)} positions), too bad {p}, right {100*(1-(sum(o.values())+p)/len(common)):.2f}%")
E = {d: set(k for k in common if sgn(V[d][k]) > sgn(X[k])) for d in files}
print("error sets: 4&5", len(E[4] & E[5]), "5&6", len(E[5] & E[6]), "4&6", len(E[4] & E[6]), "3&4", len(E[3] & E[4]), "2&4", len(E[2] & E[4]))
for a, b in ((2, 3), (3, 4), (4, 5), (5, 6)):
    print(f"values equal {a} vs {b}-ply: {sum(V[a][k] == V[b][k] for k in common)} of {len(common)}; equal to exact at {b}: {sum(V[b][k] == X[k] for k in common)}")
for k in sorted(E[6])[:12]: print(k, [V[d][k] for d in files], "exact", X[k])
