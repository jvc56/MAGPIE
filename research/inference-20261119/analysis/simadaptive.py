# Adaptive budget test on the 600-rack data: 75-rack pilot everywhere, 500 racks only where a trigger fires.
import sys,math,re,statistics as st,random
import numpy as np
sub=sys.argv[1]; BIG=int(sys.argv[2]) if len(sys.argv)>2 else 500
src=open(__file__.replace('simadaptive','simcurve')).read().split("for M in Ms:")[0].replace("Ms=[int(x) for x in sys.argv[2:]]","Ms=[]")
exec(src)
G={}
for l in open(__file__.replace('simadaptive.py','sgap.txt')):
    d=dict(re.findall(r'(\w+)=(\S+)',l)); G[(d['game'],d['turn'])]=d
PIL=75
ok=[p for p in P if predictor(p,(),PIL) is not None]
def run(fs,M):
    T=max(TE,key=lambda T:sum(score(p,T,f,M) for p,f in zip(ok,fs))); return [score(p,T,f,M) for p,f in zip(ok,fs)]
base=run([(lambda r:r['sl']) for p in ok],PIL)
small=run([predictor(p,('let',),PIL) for p in ok],PIL)
big=run([predictor(p,('let','pair'),BIG) for p in ok],BIG)
n=len(ok); gs=[a-b for a,b in zip(small,base)]; gb=[a-b for a,b in zip(big,base)]
def ci(d): return '%+.3f ±%.3f'%(st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)))
print(sub,'positions',n,'| pilot',PIL,'racks:',ci(gs),'| always',BIG,'racks:',ci(gb),'| extra from the big sample:',ci([b-a for a,b in zip(gs,gb)]))
def rmse(p):
    fit=[r for r in p[2][:PIL] if r is not None and r['sl']<=40]
    return math.sqrt(st.mean((min(r['ml'],40)-r['sl'])**2 for r in fit))
def corr(p):
    f=predictor(p,('let',),PIL); fit=[r for r in p[2][:PIL] if r is not None and r['sl']<=40]
    return st.mean(abs(f(r)-r['sl']) for r in fit)
def same(p):
    d=G[(p[0][0],p[0][1])]
    return 2.0 if float(d['gap'])>0 else (1.0 if int(d['ties'])+int(d['within2'])>0 else 0.0)
trig={'pilot disagreement (rmse)':[rmse(p) for p in ok],'size of fitted correction':[corr(p) for p in ok],'close same-tiles alternative':[same(p)+1e-3*rmse(p) for p in ok],'late bag':[-p[0][2] for p in ok]}
extra=[b-a for a,b in zip(gs,gb)]
for f in (0.25,0.5):
    k=int(n*f)
    print('big sample on %d%% of moves (average %d racks per move); random choice would give %+.3f'%(100*f,PIL+f*(BIG-PIL),st.mean(gs)+f*st.mean(extra)))
    for name,v in trig.items():
        top=set(sorted(range(n),key=lambda i:-v[i])[:k])
        mix=[gb[i] if i in top else gs[i] for i in range(n)]
        sel=[extra[i] for i in top]; rest=[extra[i] for i in range(n) if i not in top]
        print('   %-30s gain %s | extra where triggered %+.2f, elsewhere %+.2f'%(name,ci(mix),st.mean(sel),st.mean(rest)))
