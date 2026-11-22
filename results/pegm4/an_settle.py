# Replay of the stopping rule "top set equal at depth d and d+2" on per-scenario stage values (bag 2, bag-emptying candidates),
# judged by end-of-game values. Uses scenarios present at every depth and in the reference.
import collections, math
D = [2, 3, 4, 5, 6]
sg = lambda v: 1.0 if v > 0 else 0.5 if v == 0 else 0.0
V = {d: {} for d in D}; X = {}; W = {}
for d in D:
    for l in open(f"merged_end_d{d}.txt"):
        if not l.startswith("PS ") or " | " not in l: continue
        head, move, rows = l.rstrip("\n").split(" | ")
        _, idx, run, tp, fid = head.split()
        if run == "F" or (run == "T" and fid != str(d)): continue
        for r in rows.split():
            a, b, w, v = r.rsplit(":", 3); k = (int(idx), move, a, b)
            if run == "X": X[k] = int(v); W[k] = int(w)
            else: V[d][k] = int(v)
common = set(X)
for d in D: common &= set(V[d])
pos = collections.defaultdict(lambda: collections.defaultdict(list))
for k in common: pos[k[0]][k[1]].append(k)
def win(keys, src): 
    w = sum(W[k] for k in keys); return sum(W[k] * sg(src[k]) for k in keys) / w
def top(vals):
    m = max(vals.values()); return frozenset(c for c, v in vals.items() if v >= m - 1e-9)
P = {}
for i, cands in pos.items():
    if len(cands) < 2: continue
    P[i] = ({d: {c: win(ks, V[d]) for c, ks in cands.items()} for d in D}, {c: win(ks, X) for c, ks in cands.items()})
print("positions", len(P), "candidates", sum(len(p[1]) for p in P.values()), "scenarios", len(common))
def loss(tset, truth): return max(truth.values()) - sum(truth[c] for c in tset) / len(tset)
def ms(x):
    n = len(x); m = sum(x) / n; return f"{100*m:.2f} pp (SE {100*math.sqrt(sum((v-m)**2 for v in x)/max(1,n-1)/n):.2f}), positions with a loss {sum(v > 1e-9 for v in x)}"
for d in D: print(f"always stop at {d} plies: win lost", ms([loss(top(s[d]), t) for s, t in P.values()]))
for gap in (1, 2):
    stops = collections.Counter(); L = []; wrong = collections.Counter(); never = 0; depth = []
    for i, (s, t) in P.items():
        for d in D:
            if d + gap > 6: never += 1; e = 6; break
            if top(s[d]) == top(s[d + gap]): e = d + gap; break
        stops[e] += 1; depth.append(e); l = loss(top(s[e]), t); L.append(l)
        if l > 1e-9: wrong[e] += 1
    print(f"stop when top set agrees at d and d+{gap}: win lost", ms(L), f"| deepest ply searched: mean {sum(depth)/len(depth):.2f}, by depth {dict(sorted(stops.items()))}, never agreed {never}, losses by stop depth {dict(wrong)}")
# how often an agreement is a false one: agrees at d and d+2 but the top set is not the true top set
for d in (2, 3, 4):
    ag = [(s, t) for s, t in P.values() if top(s[d]) == top(s[d + 2])]
    bad = sum(loss(top(s[d + 2]), t) > 1e-9 for s, t in ag)
    print(f"agree at {d} and {d+2}: {len(ag)} of {len(P)} positions, of those with a win loss {bad}")
