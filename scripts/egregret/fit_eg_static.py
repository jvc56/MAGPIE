# Empty-bag static eval shaped by whether the opponent can go out (lab).
# usage: python3 fit_eg_static.py <egstuck output> <egregret outputs...>
# Candidates are re-ranked by static equity plus a few fitted corrections
# (all zero = greedy). Models:
#   refit    out: b*opp face;  not out: a*leave face + c
#   board    a, c split by "opponent could go out on the current board"
#   after    a, c split by "opponent can go out after this move"
#   after+   after, plus d*(opponent's best out score) when they can go out
#            and e*(opponent's best score) when they cannot
#   2-ply-ish after+ with every term free by the opponent's flag
# Loss: softmax expected 4-ply regret; held-out regret by 5 folds on games.
import math, sys
import numpy as np

FACE = dict(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ?",
                [1, 3, 3, 2, 1, 4, 2, 4, 1, 8, 5, 1, 3, 1, 1, 3, 10, 1, 1, 1, 1, 4, 4, 8, 4, 10, 0]))
def face(tiles):
    return sum(FACE.get(c, 0) for c in tiles)

board_out, reply = {}, {}
for line in open(sys.argv[1]):
    f = line.split()
    if f[0] == "S" and f[1][0] == "o":
        board_out[f[1][1:]] = f[4] == f[5]
    elif f[0] == "O":
        reply[(f[1][1:], int(f[2]))] = (int(f[5]), int(f[6]), int(f[7]), int(f[8]))

pos, cur = [], {}
for path in sys.argv[2:]:
    for line in open(path):
        f = line.split()
        if not f:
            continue
        key = f"{f[1]}_{f[2]}" if len(f) > 2 else None
        if f[0] == "P" and len(f) >= 12 and key in board_out:
            racks = f[10].split("/")
            p = dict(game=int(f[1]), key=key, mt=int(f[3]), ot=int(f[4]), b4=int(f[7]),
                     opp_face=face(racks[1]), cands=[])
            cur[key] = p
            pos.append(p)
        elif f[0] == "C" and key in cur and int(f[10]) != -99999:
            r = reply.get((key, int(f[3])))
            if r is None:
                continue
            leave = "" if f[8] == "-" else f[8]
            eq = float(f[5])
            if eq == -99999:  # pass
                eq = -2.0 * face(leave) - 10.0
            cur[key]["cands"].append(dict(eq=eq, out=int(f[7]), lf=face(leave), v4=int(f[10]),
                                          v2=int(f[9]), over=r[0], oo=r[1], obo=r[2], obs=r[3]))
pos = [p for p in pos if p["cands"]]

def feats(model, p, c):
    if c["out"]:
        return {"out_oppface": p["opp_face"]}
    lf = c["lf"]
    if model == "refit":
        return {"face": lf, "c": 1}
    if model == "board":
        s = "B1" if board_out[p["key"]] else "B0"
        return {"face|" + s: lf, "c|" + s: 1}
    s = "A1" if c["oo"] else "A0"
    d = {"face|" + s: lf, "c|" + s: 1}
    if model in ("after+", "2-ply-ish"):
        if c["oo"]:
            d["oppout_score"] = c["obo"]
        else:
            d["oppbest_score"] = c["obs"]
    if model == "2-ply-ish":
        d["oppface|" + s] = p["opp_face"]
        d["n|" + s] = len(str(lf)) * 0 + 1.0 * (p["mt"])
    return d

off = np.zeros(len(pos) + 1, int)
off[1:] = np.cumsum([len(p["cands"]) for p in pos])
pid = np.repeat(np.arange(len(pos)), np.diff(off))
eq = np.array([c["eq"] for p in pos for c in p["cands"]])
reg = np.array([p["b4"] - c["v4"] for p in pos for c in p["cands"]], float)
games = sorted(set(p["game"] for p in pos))
gfold = {g: i % 5 for i, g in enumerate(games)}
folds = np.array([gfold[p["game"]] for p in pos])

def picks(u, ks):
    return np.array([reg[off[k] + int(np.argmax(u[off[k]:off[k + 1]]))] for k in ks])

def fit(X, train, lam=0.05, temp=1.0, iters=2500):
    m = np.isin(pid, train)
    Xt, rt, pt, et = X[m], reg[m], pid[m], eq[m]
    w = np.zeros(X.shape[1]); mw = np.zeros_like(w); vw = np.zeros_like(w)
    for it in range(1, iters + 1):
        z = (et + Xt @ w) / temp
        zmax = np.full(len(pos), -1e18); np.maximum.at(zmax, pt, z)
        e = np.exp(z - zmax[pt])
        den = np.zeros(len(pos)); np.add.at(den, pt, e)
        pr = e / den[pt]
        er = np.zeros(len(pos)); np.add.at(er, pt, pr * rt)
        g = Xt.T @ (pr * (rt - er[pt]) / temp) / len(train) + 2 * lam * w / len(train)
        mw = 0.9 * mw + 0.1 * g; vw = 0.999 * vw + 0.001 * g * g
        w -= 0.05 * (mw / (1 - 0.9 ** it)) / (np.sqrt(vw / (1 - 0.999 ** it)) + 1e-8)
    return w

greedy = picks(eq, range(len(pos)))
def show(name, r, extra=""):
    d = r - greedy
    print(f"{name:26s} regret {r.mean():5.2f} (SE {r.std(ddof=1) / math.sqrt(len(r)):.2f}) wrong {100 * np.mean(r > 0):4.1f}%"
          f"  vs greedy {d.mean():+5.2f} (SE {d.std(ddof=1) / math.sqrt(len(r)):.2f}) {extra}")
print(f"positions {len(pos)}, games {len(games)}, candidates {len(eq)}")
n_oo = sum(c["oo"] for p in pos for c in p["cands"] if not c["out"])
n_no = sum(1 for p in pos for c in p["cands"] if not c["out"])
print(f"opponent can go out after a non-out candidate: {100 * n_oo / n_no:.1f}%; on the current board: "
      f"{100 * np.mean([board_out[p['key']] for p in pos]):.1f}%")
show("greedy (static equity)", greedy)
show("2-ply pick of top cands", picks(np.array([c["v2"] for p in pos for c in p["cands"]], float), range(len(pos))))
show("best of top cands", np.array([reg[off[k]:off[k + 1]].min() for k in range(len(pos))]))
# no fitting: opponent's out play is charged in full when they can go out
rule = np.array([c["eq"] - (c["obo"] if (c["oo"] and not c["out"]) else 0) for p in pos for c in p["cands"]])
show("rule: minus opp out score", picks(rule, range(len(pos))))
for model in ["after", "after+"]:
    index = {}
    rows = [feats(model, p, c) for p in pos for c in p["cands"]]
    for r in rows:
        for k in r:
            index.setdefault(k, len(index))
    X = np.zeros((len(rows), len(index)))
    for i, r in enumerate(rows):
        for k, v in r.items():
            X[i, index[k]] = v
    held = np.zeros(len(pos))
    for fold in range(5):
        w = fit(X, np.where(folds != fold)[0])
        test = np.where(folds == fold)[0]
        held[test] = picks(eq + X @ w, test)
    show(model, held)
    if model in ("after", "after+"):
        ufull = np.zeros(len(eq))
        for fold in range(5):
            w = fit(X, np.where(folds != fold)[0])
            m = np.isin(pid, np.where(folds == fold)[0])
            ufull[m] = eq[m] + X[m] @ w
        rank = np.concatenate([np.arange(off[k + 1] - off[k]) for k in range(len(pos))])
        for top in (2, 3, 4, 6, 8):
            show(f"  {model}, top {top} only", picks(np.where(rank < top, ufull, -1e9), range(len(pos))))
    w = fit(X, np.arange(len(pos)))
    print("     " + ", ".join(f"{k} {w[i]:+.2f}" for k, i in index.items()))
    sys.stdout.flush()
