# Empty-bag leave values fitted to cut greedy regret vs 4-ply solves (lab).
# usage: python3 fit_eg_leaves.py <egregret outputs...>
# Each position's top static candidates are re-ranked by
#   u = score + out * (a * opp rack face) + (1 - out) * L(leave, opp tiles)
# (as corrections on top of the static equity, so all-zero = greedy)
# where L sums learned terms over the leave's 1-, 2- and 3-tile sub-multisets
# (duplicates included), optionally split by the opponent's tile count. No
# draw value: L sees only the kept tiles. Loss: expected 4-ply regret under a
# softmax over candidates, L2-shrunk. Held-out regret by 5-fold split on games.
import itertools, math, sys
from collections import defaultdict
import numpy as np

FACE = dict(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ?",
                [1, 3, 3, 2, 1, 4, 2, 4, 1, 8, 5, 1, 3, 1, 1, 3, 10, 1, 1, 1, 1, 4, 4, 8, 4, 10, 0]))

def face(tiles):
    return sum(FACE.get(c, 0) for c in tiles if c.isupper() or c == "?")

pos = []
cur = {}
for path in sys.argv[1:]:
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if f[0] == "P":
            racks = f[10].split("/")
            p = dict(game=(path, int(f[1])), turn=int(f[2]), mt=int(f[3]), ot=int(f[4]), b4=int(f[7]),
                     opp_face=face(racks[1]) if len(racks) > 1 else 0, cands=[])
            cur[(path, f[1], f[2])] = p
            pos.append(p)
        elif f[0] == "C":
            p = cur.get((path, f[1], f[2]))
            if p is None or int(f[10]) == -99999:
                continue
            leave = "" if f[8] == "-" else "".join(sorted(f[8]))
            p["cands"].append(dict(score=int(f[4]), eq=float(f[5]), out=int(f[7]),
                                   leave=leave, v4=int(f[10])))
pos = [p for p in pos if len(p["cands"]) >= 1]
# Situations from egstuck output (env EGF_STUCK): mover/opponent strictly stuck,
# mover can go out now, opponent could go out next on the current board.
import os
sit = {}
if os.environ.get("EGF_STUCK"):
    for line in open(os.environ["EGF_STUCK"]):
        f = line.split()
        if f[0] == "S":
            sit[f[1]] = dict(stuck=f[2] != "-" or f[3] != "-", out=f[4] == f[5])
    for p in pos:
        key = f"{p['game'][1]}_{p['turn']}"
        m, o = sit.get("m" + key), sit.get("o" + key)
        if m is None:
            p["sit"] = "na"
        elif m["stuck"]:
            p["sit"] = "mstuck"
        elif o["stuck"]:
            p["sit"] = "ostuck"
        else:
            p["sit"] = ("mout" if m["out"] else "mno") + ("_oout" if o["out"] else "_ono")

def otb(ot):
    return "o1" if ot == 1 else "o2" if ot == 2 else "o34" if ot <= 4 else "o57"

def feats(leave, ot, spec, p=None):
    out = []
    subs = []
    for k in range(1, spec["k"] + 1):
        subs += sorted(set("".join(s) for s in itertools.combinations(leave, k)))
    for s in subs:
        out.append(s)
        if spec["byopp"] and len(s) <= spec["byopp"]:
            out.append(s + "|" + otb(ot))
        if spec.get("bysit") and len(s) <= spec["bysit"]:
            out.append(s + "|" + p["sit"])
    if spec.get("size"):
        out.append(f"n{len(leave)}|{otb(ot)}")
        out.append(f"face|{otb(ot)}")
    if spec.get("bysit"):
        out.append(f"n{len(leave)}|{p['sit']}")
        out.append(f"face|{p['sit']}")
    return out

def build(spec):
    index = {"_out": 0, "_nonout": 1}
    rows = []  # per candidate: base, list of (feature idx, value)
    for p in pos:
        for c in p["cands"]:
            fl = [(0, float(p["opp_face"]))] if c["out"] else [(1, 1.0)]
            if spec.get("bysit"):
                fl.append((index.setdefault(("out|" if c["out"] else "nonout|") + p["sit"], len(index)), 1.0))
            if not c["out"]:
                for name in feats(c["leave"], p["ot"], spec, p):
                    val = float(face(c["leave"])) if name.startswith("face|") else 1.0
                    fl.append((index.setdefault(name, len(index)), val))
            rows.append((c["eq"], fl))
    return index, rows

def arrays(rows):
    base = np.array([r[0] for r in rows])
    ci, fi, fv = [], [], []
    for i, (_, fl) in enumerate(rows):
        for j, v in fl:
            ci.append(i); fi.append(j); fv.append(v)
    return base, np.array(ci), np.array(fi), np.array(fv)

def fit(train_pos, cand_off, base, ci, fi, fv, nfeat, lam, temp, iters=1500):
    # candidate rows of the training positions
    sel = np.concatenate([np.arange(cand_off[k], cand_off[k + 1]) for k in train_pos])
    mask = np.zeros(len(base), bool); mask[sel] = True
    pid = np.empty(len(base), int)
    for k in range(len(pos)):
        pid[cand_off[k]:cand_off[k + 1]] = k
    reg = np.array([p["b4"] - c["v4"] for p in pos for c in p["cands"]], float)
    m = mask[ci]
    ci_t, fi_t, fv_t = ci[m], fi[m], fv[m]
    w = np.zeros(nfeat)
    mw = np.zeros(nfeat); vw = np.zeros(nfeat)
    pen = np.full(nfeat, lam)
    lr = 0.3
    for it in range(1, iters + 1):
        u = base.copy()
        np.add.at(u, ci_t, w[fi_t] * fv_t)
        z = u / temp
        zmax = np.full(len(pos), -1e18); np.maximum.at(zmax, pid[mask], z[mask])
        e = np.zeros(len(base)); e[mask] = np.exp(z[mask] - zmax[pid[mask]])
        den = np.zeros(len(pos)); np.add.at(den, pid[mask], e[mask])
        pr = np.zeros(len(base)); pr[mask] = e[mask] / den[pid[mask]]
        er = np.zeros(len(pos)); np.add.at(er, pid[mask], pr[mask] * reg[mask])
        # d(sum_i p_i r_i)/du_j = p_j (r_j - E r) / temp
        gu = np.zeros(len(base)); gu[mask] = pr[mask] * (reg[mask] - er[pid[mask]]) / temp
        g = np.zeros(nfeat); np.add.at(g, fi_t, gu[ci_t] * fv_t)
        g = g / len(train_pos) + 2 * pen * w / len(train_pos)
        mw = 0.9 * mw + 0.1 * g; vw = 0.999 * vw + 0.001 * g * g
        w -= lr * (mw / (1 - 0.9 ** it)) / (np.sqrt(vw / (1 - 0.999 ** it)) + 1e-8)
    return w

def pick_regret(ks, u, cand_off):
    out = []
    for k in ks:
        a, b = cand_off[k], cand_off[k + 1]
        j = a + int(np.argmax(u[a:b]))  # first max = static order on ties
        out.append(pos[k]["b4"] - pos[k]["cands"][j - a]["v4"])
    return np.array(out, float)

cand_off = np.zeros(len(pos) + 1, int)
cand_off[1:] = np.cumsum([len(p["cands"]) for p in pos])
games = sorted(set(p["game"] for p in pos))
fold_of_game = {g: i % 5 for i, g in enumerate(games)}
folds = np.array([fold_of_game[p["game"]] for p in pos])
print(f"positions {len(pos)}, games {len(games)}, candidates {cand_off[-1]}")

greedy = pick_regret(range(len(pos)), np.array([c["eq"] for p in pos for c in p["cands"]]), cand_off)
best = np.array([p["b4"] - max(c["v4"] for c in p["cands"]) for p in pos], float)
def show(name, r):
    print(f"{name:34s} regret {r.mean():5.2f} (SE {r.std(ddof=1) / math.sqrt(len(r)):.2f})  wrong {100 * np.mean(r > 0):4.1f}%"
          f"  vs greedy {np.mean(r - greedy):+5.2f} (SE {np.std(r - greedy, ddof=1) / math.sqrt(len(r)):.2f})")
show("greedy (static equity)", greedy)
show("best of top cands (floor)", best)

SPECS = [
    ("constant only", dict(k=0, byopp=0)),
    ("size+face by opp tiles", dict(k=0, byopp=0, size=True)),
    ("singles", dict(k=1, byopp=0, size=True)),
    ("singles by opp", dict(k=1, byopp=1, size=True)),
    ("+pairs", dict(k=2, byopp=1, size=True)),
    ("+pairs by opp", dict(k=2, byopp=2, size=True)),
    ("+triples", dict(k=3, byopp=2, size=True)),
    ("singles by sit", dict(k=1, byopp=1, size=True, bysit=1)),
    ("+pairs by sit", dict(k=2, byopp=1, size=True, bysit=2)),
    ("+pairs, singles by sit", dict(k=2, byopp=1, size=True, bysit=1)),
]
lam = float(os.environ.get("EGF_LAM", "3"))
temp = float(os.environ.get("EGF_TEMP", "3"))
only = os.environ.get("EGF_SPECS")
if only:
    SPECS = [s for s in SPECS if s[0] in only.split(",")]
print(f"lam {lam} temp {temp}")
for name, spec in SPECS:
    index, rows = build(spec)
    base, ci, fi, fv = arrays(rows)
    held = np.zeros(len(pos))
    for fold in range(5):
        train = np.where(folds != fold)[0]
        test = np.where(folds == fold)[0]
        w = fit(train, cand_off, base, ci, fi, fv, len(index), lam, temp)
        u = base.copy(); np.add.at(u, ci, w[fi] * fv)
        held[test] = pick_regret(test, u, cand_off)
    show(f"{name} ({len(index)} terms)", held)
    sys.stdout.flush()
    if os.environ.get("EGF_SHOW") and name in os.environ["EGF_SHOW"].split(","):
        w = fit(np.arange(len(pos)), cand_off, base, ci, fi, fv, len(index), lam, temp)
        inv = {v: k for k, v in index.items()}
        order = np.argsort(w)
        print("   out coef %.2f  nonout const %.2f" % (w[0], w[1]))
        print("   most negative:", ", ".join(f"{inv[i]} {w[i]:+.1f}" for i in order[:14] if i > 1))
        print("   most positive:", ", ".join(f"{inv[i]} {w[i]:+.1f}" for i in order[::-1][:14] if i > 1))
