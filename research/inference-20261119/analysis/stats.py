import sys, re, math, glob, statistics as st
d = sys.argv[1]
games = {}
pairs = set()
for p in sorted(glob.glob(d + '/worker-*.log')):
    for line in open(p, errors='replace'):
        if line.startswith('INFMATCH_GAME '):
            m = re.match(r'INFMATCH_GAME seed=(\d+) game=(\d) spread=(-?[\d.]+) .*control_emergency_passes=\d+\s*$', line)
            if m: games[(int(m[1]), int(m[2]))] = float(m[3])
        elif line.startswith('INFMATCH_PAIR '):
            m = re.match(r'INFMATCH_PAIR seed=(\d+) win_fraction=[\d.]+ spread=-?[\d.]+\s*$', line)
            if m: pairs.add(int(m[1]))
def win(s): return 1.0 if s > 0 else 0.5 if s == 0 else 0.0
def sig(s): return 1/(1+math.exp(-s/100))
def util(s): return (win(s) + 0.5*sig(s))/1.5
W, S, U, G = [], [], [], []
for seed in sorted(pairs):
    a, b = games[(seed,0)], games[(seed,1)]
    W.append((win(a)+win(b))/2); S.append((a+b)/2); U.append((util(a)+util(b))/2); G.append((sig(a)+sig(b))/2)
n = len(W)
def rep(name, x, null):
    m = st.mean(x); sd = st.stdev(x); se = sd/math.sqrt(n)
    print(f'{name}: mean={m:.5f} sd={sd:.5f} se={se:.5f} ci95=[{m-1.96*se:.5f}, {m+1.96*se:.5f}] z={(m-null)/se:+.2f}')
print('pairs', n)
rep('win', W, 0.5); rep('spread', S, 0); rep('sigmoid_spread', G, 0.5); rep('utility', U, 0.5)
