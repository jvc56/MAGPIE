# Regret of greedy endgame picks (lab, not for merge).
# usage: python3 analyze.py <egregret outputs...>
# Candidate 0 is the greedy (top static equity) pick. Regret at depth d =
# the position's best value at d (over all moves) minus the pick's value at d.
import math, sys
from collections import defaultdict

pos = {}
for path in sys.argv[1:]:
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if f[0] == "P":
            key = (path, f[1], f[2])
            pos[key] = dict(mt=int(f[3]), ot=int(f[4]), b2=int(f[6]), b4=int(f[7]),
                            b6=int(f[8]), cands=[])
        elif f[0] == "C":
            key = (path, f[1], f[2])
            if key in pos:
                pos[key]["cands"].append(dict(score=int(f[4]), eq=float(f[5]), tiles=int(f[6]),
                                              out=int(f[7]), v2=int(f[9]), v4=int(f[10]),
                                              v6=int(f[11])))

def stats(xs):
    n = len(xs)
    if n == 0:
        return "n=0"
    m = sum(xs) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / max(1, n - 1))
    nz = sum(1 for x in xs if x > 0) / n
    return f"n={n:6d} mean={m:6.2f} (SE {sd / math.sqrt(n):.2f}) wrong={100 * nz:5.1f}%"

rows = defaultdict(lambda: defaultdict(list))
for p in pos.values():
    c = p["cands"]
    if not c:
        continue
    tiles = p["mt"] + p["ot"]
    band = "all"
    bands = [band, f"mover {p['mt']}", "total<=8" if tiles <= 8 else ("total 9-11" if tiles <= 11 else "total 12-14")]
    g = c[0]
    best2_in = max(range(len(c)), key=lambda i: (c[i]["v2"], -i))
    for b in bands:
        rows[b]["greedy, 2-ply judge"].append(p["b2"] - g["v2"])
        rows[b]["greedy, 4-ply judge"].append(p["b4"] - g["v4"])
        rows[b]["2-ply pick (top cands), 4-ply judge"].append(p["b4"] - c[best2_in]["v4"])
        rows[b]["best of top cands, 4-ply judge"].append(p["b4"] - max(x["v4"] for x in c))
        if p["b6"] != -99999:
            best4_in = max(range(len(c)), key=lambda i: (c[i]["v4"], -i))
            rows[b]["greedy, 6-ply judge"].append(p["b6"] - g["v6"])
            rows[b]["4-ply pick (top cands), 6-ply judge"].append(p["b6"] - c[best4_in]["v6"])
for b in sorted(rows, key=lambda s: (s != "all", s)):
    print(f"== {b}")
    for k, xs in rows[b].items():
        print(f"  {k:40s} {stats(xs)}")
