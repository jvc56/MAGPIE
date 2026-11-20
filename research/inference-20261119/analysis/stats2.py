import sys, re, math, glob, statistics as st
def load(d):
    games = {}; pairs = set(); extra = {'sf':0,'cf':0,'sc':0,'cc':0,'si':0,'ci':0,'ss':0,'cs':0,'n':0,'skips':0,'ep':0}
    for p in sorted(glob.glob(d + '/worker-*.log')):
        for line in open(p, errors='replace'):
            if line.startswith('INFMATCH_GAME '):
                if not line.rstrip().split(' ')[-1].startswith('control_emergency_passes='): continue
                r = dict(x.split('=') for x in line.split()[1:])
                games[(int(r['seed']), int(r['game']))] = float(r['spread'])
                extra['n']+=1; extra['sf']+=int(r['soft_fallbacks']); extra['cf']+=int(r['control_fallbacks'])
                extra['sc']+=int(r['soft_clock_stops']); extra['cc']+=int(r['control_clock_stops'])
                extra['si']+=int(r['soft_iterations']); extra['ci']+=int(r['control_iterations'])
                extra['ss']+=float(r['soft_seconds']); extra['cs']+=float(r['control_seconds'])
                extra['skips']+=int(r['infer_skips']); extra['ep']+=int(r['soft_emergency_passes'])+int(r['control_emergency_passes'])
            elif line.startswith('INFMATCH_PAIR '):
                m = re.match(r'INFMATCH_PAIR seed=(\d+) win_fraction=[\d.]+ spread=-?[\d.]+\s*$', line)
                if m: pairs.add(int(m[1]))
    return games, pairs, extra
def win(s): return 1.0 if s > 0 else 0.5 if s == 0 else 0.0
def sig(s): return 1/(1+math.exp(-s/100))
def util(s): return (win(s) + 0.5*sig(s))/1.5
def rows(games, pairs):
    W,S,U=[],[],[]
    for seed in sorted(pairs):
        a,b=games[(seed,0)],games[(seed,1)]
        W.append((win(a)+win(b))/2); S.append((a+b)/2); U.append((util(a)+util(b))/2)
    return W,S,U
def rep(tag, W,S,U):
    n=len(W); print(f'{tag}: pairs={n}')
    for name,x,null in (('win',W,.5),('spread',S,0),('utility',U,.5)):
        m=st.mean(x); sd=st.stdev(x); se=sd/math.sqrt(n)
        print(f'  {name}: mean={m:.5f} sd={sd:.5f} se={se:.5f} ci95=[{m-1.96*se:.5f}, {m+1.96*se:.5f}] z={(m-null)/se:+.2f}')
AW,AS,AU=[],[],[]
for d in sys.argv[1:]:
    g,p,e=load(d); W,S,U=rows(g,p); rep(d,W,S,U); AW+=W;AS+=S;AU+=U
    n=e['n']; print(f"  games={n} soft_it={e['si']/n:.0f} ctl_it={e['ci']/n:.0f} soft_s={e['ss']/n:.3f} ctl_s={e['cs']/n:.3f} soft_fb={e['sf']} ctl_fb={e['cf']} soft_stops={e['sc']} ctl_stops={e['cc']} skips/game={e['skips']/n:.3f} emergency={e['ep']}")
if len(sys.argv)>2: rep('POOLED',AW,AS,AU)
