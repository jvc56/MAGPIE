# Engine replay of sim-steered weights. usage: steer_fit.py <out file>...
import re,math,sys,statistics as st
G=[0.25,0.35,0.5,0.7,1.0,1.4]
EPS=1e-12
for f in sys.argv[1:]:
    rows=[]
    for l in open(f):
        if not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'pm' not in d: continue
        ps=[float(x) for x in d['ps'].split(',')]; pm=[float(x) for x in d['pm'].split(',')]
        rows.append((int(d['bag']),int(d['leave']),float(d['seconds']),int(d['fitted']),ps,pm,int(d['game'])))
    if not rows: continue
    ln=lambda p:math.log(max(p,EPS))
    def ci(x,g):
        m=st.mean(x); import collections; r=collections.defaultdict(float)
        for v,gg in zip(x,g): r[gg]+=v-m
        n=len(r); return 1.96*math.sqrt(n/(n-1)*sum(v*v for v in r.values()))/len(x)
    print(f'== {f.split("/")[-1]} | positions {len(rows)} | mean steer seconds {st.mean(r[2] for r in rows):.3f} (p90 {sorted(r[2] for r in rows)[int(.9*len(rows))]:.3f}) | not fitted {sum(r[3]==0 for r in rows)}')
    def table(name,R):
        if len(R)<30: return
        g=[r[6] for r in R]
        base=[ln(r[4][4]) for r in R]   # static at the schedule (scale 1.0)
        out=[]
        for i,sc in enumerate(G):
            dm=[ln(r[5][i])-b for r,b in zip(R,base)]; ds=[ln(r[4][i])-b for r,b in zip(R,base)]
            out.append((sc,st.mean(ds),st.mean(dm),ci(dm,g)))
        best=max(out,key=lambda o:o[2])
        print(f'  {name:14s} n {len(R):4d} | '+' '.join(f'x{sc:g}: static {a:+.3f} steer {b:+.3f}' for sc,a,b,c in out)+f' | best steer x{best[0]:g} {best[2]:+.3f} ±{best[3]:.3f}')
    table('all',rows); table('bag 8+',[r for r in rows if r[0]>=8])
    for lo,hi in ((61,100),(31,60),(14,30),(8,13),(1,7)): table(f'bag {lo}-{hi}',[r for r in rows if lo<=r[0]<=hi])
    for lo,hi in ((1,2),(3,4),(5,6)): table(f'kept {lo}-{hi}',[r for r in rows if lo<=r[1]<=hi])
    # tail: how often steering cuts the truth
    i=G.index(0.7); R=[r for r in rows if r[0]>=8]
    rat=[ln(r[5][i])-ln(r[4][4]) for r in R]
    print('  bag 8+ at x0.7: truth raised %.0f%% | cut >2x %.1f%% | cut >10x %.1f%% | raised >2x %.1f%%'%(100*sum(v>0 for v in rat)/len(R),100*sum(v<-0.693 for v in rat)/len(R),100*sum(v<-2.303 for v in rat)/len(R),100*sum(v>0.693 for v in rat)/len(R)))
