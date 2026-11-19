# Leave values together with the per-candidate out-play check (lab).
# usage: python3 fit_eg_combo.py <egstuck all output> <egregret output>
# Top-12 candidates only (those with 4-ply values). Held-out by 5 folds on games.
import sys, math, itertools, os
from collections import defaultdict
import numpy as np
FACE = dict(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ?", [1,3,3,2,1,4,2,4,1,8,5,1,3,1,1,3,10,1,1,1,1,4,4,8,4,10,0]))
P, C = {}, {}
for l in open(sys.argv[2]):
    f = l.split()
    if not f: continue
    if f[0] == "P" and len(f) >= 12:
        P[f[1] + "_" + f[2]] = dict(game=int(f[1]), b4=int(f[7]), ot=int(f[4]))
    elif f[0] == "C" and int(f[10]) != -99999:
        C[(f[1] + "_" + f[2], int(f[3]))] = int(f[10])
cands = defaultdict(list)
for l in open(sys.argv[1]):
    f = l.split()
    if f[0] != "O": continue
    k = f[1][1:]; i = int(f[2])
    if (k, i) not in C or k not in P: continue
    eq = float(f[13]); leave = "" if f[14] == "-" else "".join(sorted(f[14]))
    if eq == -99999: eq = -2 * sum(FACE[c] for c in leave) - 10
    cands[k].append(dict(eq=eq, out=int(f[5]), oo=int(f[6]), obo=int(f[7]), obs=int(f[8]), leave=leave, v4=C[(k, i)]))
keys = [k for k in cands if cands[k]]
lam = float(os.environ.get("EGF_LAM", "3")); temp = 1.0
def otb(ot): return "o1" if ot == 1 else "o2" if ot == 2 else "o34" if ot <= 4 else "o57"
def subs(leave, kmax):
    out = []
    for n in range(1, kmax + 1): out += sorted(set("".join(s) for s in itertools.combinations(leave, n)))
    return out
def build(spec):
    index = {}; base = []; ci = []; fi = []; fv = []; row = 0
    def add(name, v=1.0):
        ci.append(row); fi.append(index.setdefault(name, len(index))); fv.append(v)
    for k in keys:
        for c in cands[k]:
            b = c["eq"]
            if not c["out"]:
                if spec["rule"] and c["oo"]: b -= c["obo"]
                tag = ("oo" if c["oo"] else "no") if spec["split"] else "all"
                if spec["flag"]:
                    add("const|" + ("oo" if c["oo"] else "no"))
                    if spec["flag"] > 1:
                        add("obo", c["obo"] if c["oo"] else 0.0)
                        add("obs|no", 0.0 if c["oo"] else c["obs"])
                if spec["k"]:
                    add(f"n{len(c['leave'])}|{tag}"); add("face|" + tag, float(sum(FACE[x] for x in c["leave"])))
                    for s in subs(c["leave"], spec["k"]):
                        add(s)
                        if spec["split"]: add(s + "|" + tag)
            else:
                add("_out", 1.0)
            base.append(b); row += 1
    return index, np.array(base), np.array(ci, int), np.array(fi, int), np.array(fv)
off = np.zeros(len(keys) + 1, int); off[1:] = np.cumsum([len(cands[k]) for k in keys])
pid = np.repeat(np.arange(len(keys)), np.diff(off))
reg = np.array([P[k]["b4"] - c["v4"] for k in keys for c in cands[k]], float)
games = sorted(set(P[k]["game"] for k in keys)); fg = {g: i % 5 for i, g in enumerate(games)}
folds = np.array([fg[P[k]["game"]] for k in keys])
def fit(train, base, ci, fi, fv, nf, iters=1500):
    mask = np.isin(pid, train); m = mask[ci]; cit, fit_, fvt = ci[m], fi[m], fv[m]
    w = np.zeros(nf); mw = np.zeros(nf); vw = np.zeros(nf)
    for it in range(1, iters + 1):
        u = base.copy(); np.add.at(u, cit, w[fit_] * fvt); z = u / temp
        zmax = np.full(len(keys), -1e18); np.maximum.at(zmax, pid[mask], z[mask])
        e = np.zeros(len(base)); e[mask] = np.exp(z[mask] - zmax[pid[mask]])
        den = np.zeros(len(keys)); np.add.at(den, pid[mask], e[mask])
        pr = np.zeros(len(base)); pr[mask] = e[mask] / den[pid[mask]]
        er = np.zeros(len(keys)); np.add.at(er, pid[mask], pr[mask] * reg[mask])
        gu = np.zeros(len(base)); gu[mask] = pr[mask] * (reg[mask] - er[pid[mask]]) / temp
        g = np.zeros(nf); np.add.at(g, fit_, gu[cit] * fvt); g = (g + 2 * lam * w) / len(train)
        mw = 0.9 * mw + 0.1 * g; vw = 0.999 * vw + 0.001 * g * g
        w -= 0.3 * (mw / (1 - 0.9 ** it)) / (np.sqrt(vw / (1 - 0.999 ** it)) + 1e-8)
    return w
def picks(u):
    return np.array([reg[off[i] + int(np.argmax(u[off[i]:off[i + 1]]))] for i in range(len(keys))])
greedy = picks(np.array([c["eq"] for k in keys for c in cands[k]]))
def show(name, r):
    d = r - greedy
    print(f"{name:52s} regret {r.mean():5.2f} wrong {100*np.mean(r>0):4.1f}%  vs greedy {d.mean():+.2f} (SE {d.std(ddof=1)/math.sqrt(len(d)):.2f})", flush=True)
print(f"positions {len(keys)} lam {lam}")
show("greedy", greedy)
SPECS = [
 ("leaves only (singles+pairs), no check", dict(rule=0, flag=0, k=2, split=0)),
 ("check only: minus out score (no fit)", dict(rule=1, flag=0, k=0, split=0)),
 ("check + fitted constants", dict(rule=1, flag=1, k=0, split=0)),
 ("check + leaves (one set)", dict(rule=1, flag=1, k=2, split=0)),
 ("check + leaves split by opp can/can't go out", dict(rule=1, flag=1, k=2, split=1)),
 ("check + opp best score", dict(rule=1, flag=2, k=0, split=0)),
 ("check + opp best score + leaves split", dict(rule=1, flag=2, k=2, split=1)),
]
for name, spec in SPECS:
    index, base, ci, fi, fv = build(spec)
    if not index: show(name, picks(base)); continue
    held = np.zeros(len(keys))
    for fold in range(5):
        w = fit(np.where(folds != fold)[0], base, ci, fi, fv, len(index))
        u = base.copy(); np.add.at(u, ci, w[fi] * fv)
        held[folds == fold] = picks(u)[folds == fold]
    show(f"{name} ({len(index)} terms)", held)
    if os.environ.get("EGF_OUT") and os.environ.get("EGF_OUT_SPEC") == name:
        w = fit(np.arange(len(keys)), base, ci, fi, fv, len(index))
        with open(os.environ["EGF_OUT"], "w") as out:
            for nm, i in sorted(index.items()): out.write(f"{nm}\t{w[i]:.4f}\n")
    if os.environ.get("EGF_SHOW") == name:
        w = fit(np.arange(len(keys)), base, ci, fi, fv, len(index)); inv = {v: k for k, v in index.items()}; o = np.argsort(w)
        print("   neg:", ", ".join(f"{inv[i]} {w[i]:+.1f}" for i in o[:16])); print("   pos:", ", ".join(f"{inv[i]} {w[i]:+.1f}" for i in o[::-1][:16]))
