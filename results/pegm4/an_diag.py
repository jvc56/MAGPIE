# usage: python3 an_diag.py <pegdiag output>
# Composition of the greedy ranking (tiles played) with and without the middle
# stage, and what the 2-ply stage finished within its budget, by tiles played.
import sys, collections
G = collections.defaultdict(dict)   # (idx, arm) -> rank -> (tiles, move)
E = collections.defaultdict(list)   # (idx, arm) -> [(rank, tiles, secs, move)]
R = {}
for l in open(sys.argv[1]):
    f = l.split()
    if len(f) < 4 or f[0] != "PD": continue
    idx, arm, kind = int(f[1]), f[2], f[3]
    if kind == "G": G[(idx, arm)][int(f[4])] = (int(f[5]), " ".join(f[6:]))
    elif kind == "E": E[(idx, arm)].append((int(f[5]), int(f[6]), float(f[7]), " ".join(f[8:])))
    elif kind == "R": R[(idx, arm)] = (f[4], float(f[5]), " ".join(f[6:]))
pos = sorted(set(i for i, a in R) & set(i for i, a in G))
pos = [i for i in pos if all((i, a) in R and (i, a) in G for a in ("static", "mid"))]
print("positions", len(pos))
def cls(t): return "0 (pass/exch)" if t == 0 else "1 tile" if t == 1 else "2+ tiles"
for arm in ("static", "mid"):
    print(f"== {arm}")
    for top in (2, 8, 32):
        c = collections.Counter(); n = 0
        for i in pos:
            for r in range(top):
                if r in G[(i, arm)]: c[cls(G[(i, arm)][r][0])] += 1; n += 1
        print(f"  top {top:2d} entering 2-ply: " + ", ".join(f"{k} {100*v/n:.0f}%" for k, v in sorted(c.items())))
    # of candidates in the top 32, share finished at 2-ply within the budget, by class
    tot = collections.Counter(); fin = collections.Counter()
    for i in pos:
        done = set(m for _, _, _, m in E[(i, arm)])
        for r in range(32):
            if r in G[(i, arm)]:
                t, m = G[(i, arm)][r]; tot[cls(t)] += 1; fin[cls(t)] += m in done
    print("  finished at 2-ply in budget: " + ", ".join(f"{k} {fin[k]}/{tot[k]} ({100*fin[k]/tot[k]:.0f}%)" for k in sorted(tot)))
    st = collections.Counter(R[(i, arm)][0] for i in pos)
    nfin = [len(E[(i, arm)]) for i in pos]
    print(f"  stage result: {dict(st)}; candidates finished per position mean {sum(nfin)/len(nfin):.1f}, <2 on {sum(n < 2 for n in nfin)}")
    # how far down the order the stage got: first unfinished rank
    blk = collections.Counter()
    for i in pos:
        done = set(m for _, _, _, m in E[(i, arm)])
        for r in range(32):
            if r in G[(i, arm)] and G[(i, arm)][r][1] not in done:
                blk[cls(G[(i, arm)][r][0])] += 1; break
    print(f"  first unfinished candidate is: {dict(blk)}")
# positions where mid got <2 but static got >=2
bad = [i for i in pos if len(E[(i, 'mid')]) < 2 <= len(E[(i, 'static')])]
print("mid <2 finished but static >=2:", len(bad))
for i in bad[:6]:
    print(f"  pos {i}: static top4 tiles {[G[(i,'static')][r][0] for r in range(4) if r in G[(i,'static')]]}  mid top4 tiles {[G[(i,'mid')][r][0] for r in range(4) if r in G[(i,'mid')]]}")
