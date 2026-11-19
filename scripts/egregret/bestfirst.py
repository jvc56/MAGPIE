# Best-first out-play check over ALL moves (lab).
# usage: python3 bestfirst.py <egstuck all output> <egregret output> <V cache> [requests out]
# Rule: value = static equity - opponent's best out score after the move.
# Bound without a recheck: equity - best cached out play the move leaves alone.
import sys, os, statistics, math
from collections import defaultdict
FACE = dict(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ?", [1,3,3,2,1,4,2,4,1,8,5,1,3,1,1,3,10,1,1,1,1,4,4,8,4,10,0]))
P, V4 = {}, {}
for l in open(sys.argv[2]):
    f = l.split()
    if not f: continue
    if f[0] == "P" and len(f) >= 12: P[f[1] + "_" + f[2]] = dict(b4=int(f[7]), cgp=" ".join(f[9:]), mt=int(f[3]))
    elif f[0] == "C": V4[(f[1] + "_" + f[2], int(f[3]))] = int(f[10])
if os.path.exists(sys.argv[3]):
    for l in open(sys.argv[3]):
        f = l.split()
        if f and f[0] == "V": V4[(f[1], int(f[2]))] = int(f[3])
moves = defaultdict(list)
for l in open(sys.argv[1]):
    f = l.split()
    if f[0] != "O": continue
    k = f[1][1:]
    eq = float(f[13]); over = int(f[5]); oo = int(f[6]); obo = int(f[7]); surv = int(f[10]); bsurv = int(f[11])
    if eq == -99999: eq = -2 * sum(FACE[c] for c in f[14] if c != "-") - 10
    moves[k].append((eq, eq - (obo if oo else 0), eq - (bsurv if surv else 0)))
need, stats = [], defaultdict(list)
for k, m in moves.items():
    if k not in P: continue
    order = sorted(range(len(m)), key=lambda i: (-m[i][2], i))
    best, pick, n = -1e18, None, 0
    for i in order:
        if m[i][2] <= best: break
        n += 1
        if m[i][1] > best: best, pick = m[i][1], i
    stats["checks"].append(n)
    stats["rank"].append(pick)
    stats["moves"].append(len(m))
    # static-order version: stop when the next equity cannot win
    best2, n2 = -1e18, 0
    for i in range(len(m)):
        if m[i][0] <= best2: break
        n2 += 1; best2 = max(best2, m[i][1])
    stats["checks_eq"].append(n2)
    top12 = max(range(min(12, len(m))), key=lambda i: (m[i][1], -i))
    for name, i in (("rule, all moves", pick), ("rule, top 12", top12), ("greedy", 0)):
        if (k, i) in V4: stats[name].append((k, P[k]["b4"] - V4[(k, i)]))
        else: need.append((k, i))
c = sorted(stats["checks"]); n = len(c)
print(f"positions {n}, moves per position mean {statistics.mean(stats['moves']):.0f}")
for nm in ("checks", "checks_eq"):
    c = sorted(stats[nm])
    print(f"{nm:10s} mean {statistics.mean(c):.2f} median {c[n//2]} p90 {c[int(.9*n)]} p99 {c[int(.99*n)]} max {c[-1]}  1 check {100*sum(x==1 for x in c)/n:.0f}%  <=3 {100*sum(x<=3 for x in c)/n:.0f}%")
print(f"rule's pick outside static's top 12: {100*sum(r>=12 for r in stats['rank'])/n:.1f}%  is static's top: {100*sum(r==0 for r in stats['rank'])/n:.1f}%")
g = dict(stats["greedy"])
for name in ("greedy", "rule, top 12", "rule, all moves"):
    r = stats[name]
    if len(r) < n: print(f"{name}: {n-len(r)} values missing"); continue
    x = [v for _, v in r]; d = [v - g[k] for k, v in r]
    print(f"{name:18s} regret {statistics.mean(x):5.2f}  wrong {100*sum(v>0 for v in x)/n:4.1f}%  vs greedy {statistics.mean(d):+.2f} (SE {statistics.stdev(d)/math.sqrt(n):.2f})")
if len(sys.argv) > 4:
    with open(sys.argv[4], "w") as out:
        for k, i in sorted(set(need)): out.write(f"{k} {i} {P[k]['cgp']}\n")
    print("requests", len(set(need)))
