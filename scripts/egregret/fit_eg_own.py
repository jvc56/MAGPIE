# fit_eg_combo.py plus the mover's own going out next turn (lab).
# EGF_OWN_TAG=X uses the cheap estimates (X lines) instead of the exact W lines.
# usage: python3 fit_eg_own.py <egstuck output with W lines (EGS_OWN_OUTS=1)> <egregret output>
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
W = {}
for l in open(sys.argv[1]):
    f = l.split()
    if f and f[0] == os.environ.get("EGF_OWN_TAG", "W"): W[(f[1][1:], int(f[2]))] = [int(x) for x in f[3:]]
ZERO = [0] * 9
for l in open(sys.argv[1]):
    f = l.split()
    if not f or f[0] != "O": continue
    k = f[1][1:]; i = int(f[2])
    if (k, i) not in C or k not in P: continue
    eq = float(f[13]); leave = "" if f[14] == "-" else "".join(sorted(f[14]))
    if eq == -99999: eq = -2 * sum(FACE[c] for c in leave) - 10
    cands[k].append(dict(eq=eq, out=int(f[5]), oo=int(f[6]), obo=int(f[7]), obs=int(f[8]), surv=int(f[10]), survb=int(f[11]), leave=leave, v4=C[(k, i)], own=W.get((k, i), ZERO)))
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
            if spec.get("cheap"):
                # The opponent's out plays known before the move that it leaves standing.
                c = dict(c, oo=1 if c["surv"] > 0 else 0, obo=c["survb"])
            b = c["eq"]
            if not c["out"]:
                if spec["rule"] and c["oo"]: b -= c["obo"]
                tag = ("oo" if c["oo"] else "no") if spec["split"] else "all"
                if spec["flag"]:
                    add("const|" + ("oo" if c["oo"] else "no"))
                    if spec["flag"] > 1:
                        add("obo", c["obo"] if c["oo"] else 0.0)
                        add("obs|no", 0.0 if c["oo"] else c["obs"])
                own = spec.get("own", 0)
                if own and c["own"][0] > 0:
                    nout, bout, nlanes, nrep, nkept, leastout, topkeep, topout, net = c["own"]
                    t = ("oo" if c["oo"] else "no") if spec.get("owntag", 1) else "all"
                    add("own|" + t); add("ownb|" + t, float(bout)); add("ownl2|" + t, 1.0 if nlanes >= 2 else 0.0)
                    add("ownl3|" + t, 1.0 if nlanes >= 3 else 0.0)
                    if spec.get("ownleave"):
                        add("ownface|" + t, float(sum(FACE[x] for x in c["leave"]))); add(f"ownn{len(c['leave'])}|" + t)
                    if own > 1 and nrep > 0:
                        add("safe|" + t, 1.0 if nkept == nrep else 0.0); add("safeb|" + t, float(leastout))
                        add("keepfrac|" + t, nkept / nrep); add("topkeep|" + t, float(topkeep)); add("topout|" + t, float(topout))
                        add("net|" + t, float(net))
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
 ("cheap: check by standing opp outs (no fit)", dict(cheap=1, rule=1, flag=0, k=0, split=0)),
 ("cheap: check + constants", dict(cheap=1, rule=1, flag=1, k=0, split=0)),
 ("cheap: check + own outs", dict(cheap=1, rule=1, flag=1, k=0, split=0, own=1)),
 ("cheap: check + own outs + leave size/face", dict(cheap=1, rule=1, flag=1, k=0, split=0, own=1, ownleave=1)),
 ("cheap: check + own outs + survive", dict(cheap=1, rule=1, flag=1, k=0, split=0, own=2)),
 ("cheap: check + own outs + survive + leaves split", dict(cheap=1, rule=1, flag=1, k=2, split=1, own=2)),
 ("exactopp: check + constants", dict(rule=1, flag=1, k=0, split=0)),
 ("static + own outs, untagged (4 terms, no opp gen)", dict(rule=0, flag=0, k=0, split=0, own=1, owntag=0)),
 ("static + own outs + leave size/face, untagged", dict(rule=0, flag=0, k=0, split=0, own=1, owntag=0, ownleave=1)),
 ("static + own outs + survive, untagged", dict(rule=0, flag=0, k=0, split=0, own=2, owntag=0)),
 ("check only: minus out score (no fit)", dict(rule=1, flag=0, k=0, split=0)),
 ("static + own outs on the board (no reply look)", dict(rule=0, flag=0, k=0, split=0, own=1)),
 ("static + own outs + survive replies", dict(rule=0, flag=0, k=0, split=0, own=2)),
 ("check + own outs on the board", dict(rule=1, flag=1, k=0, split=0, own=1)),
 ("check + own outs + survive replies", dict(rule=1, flag=1, k=0, split=0, own=2)),
 ("check + opp best score + leaves split", dict(rule=1, flag=2, k=2, split=1)),
 ("check + opp best + leaves split + own outs on board", dict(rule=1, flag=2, k=2, split=1, own=1)),
 ("check + opp best + leaves split + own outs + survive", dict(rule=1, flag=2, k=2, split=1, own=2)),
]
for name, spec in SPECS:
    if os.environ.get("EGF_ONLY") and os.environ["EGF_ONLY"] not in name: continue
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
    if os.environ.get("EGF_SHOW") and os.environ["EGF_SHOW"] in name:
        w = fit(np.arange(len(keys)), base, ci, fi, fv, len(index)); inv = {v: k for k, v in index.items()}; o = np.argsort(w)
        print("   weights:", ", ".join(f"{inv[i]} {w[i]:+.2f}" for i in o))
