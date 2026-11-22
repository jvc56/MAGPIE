# usage: python3 an_tl.py <pegtieloss output>
# Loss from the order of candidates tied at the top of a timed solve, judged by end-of-game values.
import sys, math, collections
tc = collections.defaultdict(list)   # idx -> [(rank, tiles, spread_unknown, spread, move)]
tl = {}
X = collections.defaultdict(dict)    # idx -> move -> (win, spread, weight)
for l in open(sys.argv[1]):
    l = l.rstrip("\n")
    if l.startswith("TC "):
        head, move = l.split(" | ")
        _, idx, rank, tiles, su, sp = head.split()
        tc[int(idx)].append((int(rank), int(tiles), int(su), float(sp), move))
    elif l.startswith("TL ") and "FAIL" not in l:
        f = l.split()
        tl[int(f[1])] = (f[2], float(f[3]), int(f[4]), int(f[5]), float(f[6]))
    elif l.startswith("PS ") and " | " in l:
        head, move, rows = l.split(" | ")
        idx = int(head.split()[1])
        w = wins = sp = 0
        for r in rows.split():
            a, b, wt, v = r.rsplit(":", 3)
            wt = int(wt); v = int(v)
            w += wt; wins += wt * (1 if v > 0 else 0.5 if v == 0 else 0); sp += wt * v
        if w: X[idx][move] = (wins / w, sp / w, w)
def util(win, sp): return 100 * (win + 0.5 / (1 + math.exp(-sp / 100))) / 1.5
n = len(tl); tied_pos = [i for i in tl if tl[i][2] >= 2]
print(f"positions {n}, tied at top {len(tied_pos)}, of those without spreads {sum(tl[i][3] for i in tied_pos)}")
rows = []; nojudge = pick_unjudged = 0
for i in tied_pos:
    cands = sorted(tc[i])
    full = max((v[2] for v in X[i].values()), default=0)
    j = {m: v for m, v in X[i].items() if v[2] == full}
    if len(j) < 2: nojudge += 1; continue
    pick = cands[0][4]
    if pick not in j: pick_unjudged += 1; continue
    us = {m: util(v[0], v[1]) for m, v in j.items()}
    best = max(us, key=us.get)
    known = [c for c in cands if c[4] in j]
    bysp = max(known, key=lambda c: c[3])[4]
    rows.append((i, len(j), tl[i][3], us[best] - us[pick], sum(us.values()) / len(us) - us[pick],
                 j[best][0] - j[pick][0], j[best][1] - j[pick][1], us[best] - us[bysp], pick == best))
print(f"judged {len(rows)} (judge incomplete {nojudge}, pick not judged {pick_unjudged})")
def ms(v):
    if not v: return "n/a"
    m = sum(v) / len(v); s = math.sqrt(sum((x - m) ** 2 for x in v) / max(1, len(v) - 1) / len(v))
    return f"{m:+.2f} (SE {s:.2f})"
for name, sel in (("all judged", rows), ("no spreads", [r for r in rows if r[2]]), ("spreads known", [r for r in rows if not r[2]])):
    if not sel: continue
    print(f"{name}: n={len(sel)}, pick is the best {sum(r[8] for r in sel)}")
    print("   utility lost vs best tied:", ms([r[3] for r in sel]))
    print("   utility: mean tied minus pick:", ms([r[4] for r in sel]))
    print("   win lost vs best tied (pp):", ms([100 * r[5] for r in sel]))
    print("   spread lost vs best tied:", ms([r[6] for r in sel]))
    print("   utility lost if ordered by the solve's own spread:", ms([r[7] for r in sel]))
    print(f"   per all {n} positions: utility lost {sum(r[3] for r in sel) / n:.3f}")
for r in sorted(rows, key=lambda r: -r[3])[:8]:
    print("   worst:", r[0], f"judged {r[1]} lost util {r[3]:.2f} win {100*r[5]:.1f}pp spread {r[6]:.1f}")
