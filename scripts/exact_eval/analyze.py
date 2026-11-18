# usage: analyze.py <dir> <judge tag> <block dirs...>
# Gain of pick 1 over pick 0 per eligible position (agreeing positions are 0),
# with the standard error clustered by source game.
import glob, math, re, sys
from collections import defaultdict
base, tag, blocks = sys.argv[1], sys.argv[2], sys.argv[3:]
games_n = defaultdict(int)          # game -> eligible positions
per_pos = defaultdict(list)         # pos -> [(dspread, dwp)]
tasks = set()
for b in blocks:
    done = set()
    data = defaultdict(dict)
    for path in glob.glob(f"{base}/{b}/nest_{tag}_*.out"):
        for line in open(path):
            m = re.match(r"NEST pos=(\d+) pick=(\d) sample=(\d+) value=(\S+) wp=(\S+)", line)
            if m:
                data[(int(m[1]), int(m[3]))][int(m[2])] = (float(m[4]), float(m[5]))
    if not data:
        continue
    for line in open(f"{base}/{b}/simx.out"):
        m = re.match(r"SIMX pos idx=(\d+) game=(\d+)", line)
        if m:
            games_n[int(m[2])] += 1
    for line in open(f"{base}/{b}/nest_tasks.tsv"):
        tasks.add(int(line.split("\t")[0]))
    for (pos, _), p in data.items():
        if len(p) == 2:
            per_pos[pos].append((p[1][0] - p[0][0], 100 * (p[1][1] - p[0][1])))
n = sum(games_n.values())
missing = len(tasks - set(per_pos))
samples = sum(len(v) for v in per_pos.values())
print(f"judge={tag}: {n} positions in {len(games_n)} games; {len(tasks)} differing picks "
      f"({100*len(tasks)/max(1,n):.1f}%), {missing} not yet judged; {samples} paired samples")
for col, name in ((0, "spread"), (1, "win %")):
    game_sum = defaultdict(float)
    noise = 0.0
    for pos, d in per_pos.items():
        xs = [x[col] for x in d]
        mean = sum(xs) / len(xs)
        game_sum[pos // 100] += mean
        if len(xs) > 1:
            noise += sum((x - mean) ** 2 for x in xs) / (len(xs) - 1) / len(xs)
    total = sum(game_sum.values())
    est = total / n
    g = len(games_n)
    resid = sum((game_sum.get(k, 0.0) - est * games_n[k]) ** 2 for k in games_n)
    se = math.sqrt(g / (g - 1) * resid) / n if g > 1 else float("nan")
    cond = total / max(1, len(per_pos))
    print(f"  {name}: {est:+.4f} per move (SE {se:.4f} by game; sampling noise alone {math.sqrt(noise)/n:.4f}); "
          f"{cond:+.3f} on differing positions")
