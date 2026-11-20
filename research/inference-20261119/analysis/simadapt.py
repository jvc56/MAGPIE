# Does sim/static agreement on a sample of racks tell us what temperature to use for static? usage: simadapt.py <subdir or ''> <M>
import sys,math,statistics as st
import numpy as np
M=int(sys.argv[2]); sub=sys.argv[1]
src=open(__file__.replace('simadapt','simimpute3')).read().split("def pairs(")[0].replace("Ms=[int(x) for x in sys.argv[2:]]","Ms=[]")
exec(src)
TE=(0.5,1,1.5,2,3,4,6,8,12,16)
rows=[]
for p in P:
    fit=[r for r in p[2][:M] if r is not None and r['sl']<=40]
    if len(fit)<8: continue
    x=np.array([r['sl'] for r in fit]); y=np.array([min(r['ml'],40.0) for r in fit])
    rmse=float(np.sqrt(np.mean((y-x)**2)))            # raw disagreement, points
    b=float(np.dot(x,y)/max(np.dot(x,x),1e-9))        # slope through origin
    rows.append((p,rmse,b))
def score(p,T):
    held=p[2][M:]
    L=lambda r: 0.0 if r is None else math.exp(-r['sl']/T)
    lt=L(p[1]); tot=lt+sum(L(r) for r in held)
    return math.log((len(held)+1)*lt/tot) if lt>0 else -20.0
def ci(d): return '%+.3f ±%.3f'%(st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)))
T0=max(TE,key=lambda T:sum(score(p,T) for p,_,_ in rows)); base=[score(p,T0) for p,_,_ in rows]
print(sub or 'main (500 rollouts)','positions',len(rows),'| one temperature for all: T',T0,'mean %+.3f'%st.mean(base))
rs=sorted(r[1] for r in rows); cut=[rs[len(rs)//3],rs[2*len(rs)//3]]
print('by sim/static disagreement on the %d sampled racks:'%M)
adapt=[]
for name,lo,hi in (('agree (rmse < %.1f)'%cut[0],-1,cut[0]),('middle',cut[0],cut[1]),('disagree (rmse > %.1f)'%cut[1],cut[1],1e9)):
    g=[(p,b) for p,r,b in rows if lo<r<=hi]
    T=max(TE,key=lambda T:sum(score(p,T) for p,_ in g))
    a=[score(p,T0) for p,_ in g]; c=[score(p,T) for p,_ in g]
    print('  %-24s n %4d | best static T %-4g | static at common T %+.3f, at own T %+.3f | median slope %.2f'%(name,len(g),T,st.mean(a),st.mean(c),st.median(b for _,b in g)))
    adapt+= [y-x for x,y in zip(a,c)]
print('gain from a temperature per disagreement third:',ci(adapt))
best=None
for Tb in TE:
    for c in (0,0.05,0.1,0.2,0.3,0.5,1.0):
        v=[score(p,Tb*(1+c*r)) for p,r,_ in rows]
        if best is None or st.mean(v)>st.mean(best[0]): best=(v,Tb,c)
print('continuous rule T = %g x (1 + %g x rmse):'%(best[1],best[2]),ci([a-b for a,b in zip(best[0],base)]))
best=None
for Tb in TE:
    v=[score(p,Tb/min(max(b,0.25),4)) for p,_,b in rows]
    if best is None or st.mean(v)>st.mean(best[0]): best=(v,Tb)
print('slope rule T = %g / slope:'%best[1],ci([a-b for a,b in zip(best[0],base)]))
