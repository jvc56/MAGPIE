#!/usr/bin/env python3
"""Pool patdecide shard output: per-disagreement and per-decision effects
(b minus a), overall and stratified by bag quintile and bingo class."""
import glob, math, sys, json
out, tag = sys.argv[1], sys.argv[2]
cases, positions, disagreements = [], 0, 0
for f in glob.glob(f"{out}/shard*.txt"):
    for ln in open(f):
        p = ln.rstrip("\n").split("\t")
        if p[0] == "POS":
            positions += int(p[1]); disagreements += int(p[2])
        elif p[0] == "CASE":
            cases.append(dict(seed=int(p[1]), bag=int(p[2]), cls=p[3],
                              u=float(p[4]) * 100, w=float(p[5]) * 100,
                              s=float(p[6]),
                              lead=float(p[8]) if len(p) > 8 else None))
def stats(xs):
    n = len(xs)
    if n == 0: return (0, float("nan"), float("nan"))
    m = sum(xs) / n
    se = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1) / n) if n > 1 else float("nan")
    return n, m, se
def quint(bag):
    return min(4, (bag - 1) // 17)
qlab = ["1-17", "18-34", "35-51", "52-68", "69-86"]
print(f"== {tag}: positions={positions} disagreements={disagreements} "
      f"rate={100*disagreements/max(positions,1):.3f}%")
for key, lab in (("u", "utility pp"), ("w", "win pp"), ("s", "spread pts")):
    n, m, se = stats([c[key] for c in cases])
    # per-decision: zeros for agreeing positions
    tot = sum(c[key] for c in cases)
    pd = tot / max(positions, 1)
    sq = sum(c[key] ** 2 for c in cases)
    pd_se = math.sqrt(max(sq / positions - pd * pd, 0) / positions) if positions else float("nan")
    print(f"  {lab:11s} per-disagreement {m:+.3f} ± {se:.3f} (n={n}) | "
          f"per-decision {pd:+.5f} ± {pd_se:.5f}")
print("  by bag quintile (utility pp per disagreement):")
for q in range(5):
    n, m, se = stats([c["u"] for c in cases if quint(c["bag"]) == q])
    print(f"    bag {qlab[q]:6s} n={n:5d}  {m:+.3f} ± {se:.3f}")
print("  by bingo class (utility pp per disagreement):")
for cls in ("nn", "bb", "mixed"):
    n, m, se = stats([c["u"] for c in cases if c["cls"] == cls])
    print(f"    {cls:6s} n={n:5d}  {m:+.3f} ± {se:.3f}")
leads = [c for c in cases if c["lead"] is not None]
if leads:
    print("  by mover lead before the move (utility pp per disagreement):")
    for lo, hi, lab in ((-1e9, -50, "<= -50"), (-50, -15, "-50..-15"),
                        (-15, 15, "-15..15"), (15, 50, "15..50"),
                        (50, 1e9, ">= 50")):
        n, m, se = stats([c["u"] for c in leads if lo < c["lead"] <= hi])
        print(f"    lead {lab:9s} n={n:5d}  {m:+.3f} ± {se:.3f}")
json.dump(dict(tag=tag, positions=positions, disagreements=disagreements, cases=cases),
          open(f"{out}/pooled.json", "w"))
