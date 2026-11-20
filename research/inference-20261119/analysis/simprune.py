# Offline: (1) paired gain of sim over static with CIs; (2) how much of it
# survives when only racks with an unclear static decision are simmed.
import math,statistics as st,sys
sys.argv=[sys.argv[0]]
exec(open(__file__.replace('simprune','simfit')).read().split("bands=")[0])
bands=((46,100),(21,45),(8,20),(1,7))
TE=(0.25,0.5,1,1.5,2,3,4,5,6,8,12)
def band(p):
    return next(i for i,b in enumerate(bands) if b[0]<=p[0][2]<=b[1])
def L(r,T,X,Y,lam):
    if r is None: return 0.0
    o,se,sw,sq=r; loss=se[0]-se[o]
    sim = lam>0 and loss<=X and not (o==0 and len(se)>1 and se[0]-se[1]>Y)
    v=[s+lam*(q-s) for s,q in zip(se,sq)] if sim else se
    return math.exp((v[o]-max(v))/T)
def g(p,T,X,Y,lam):
    lt=L(p[1],T,X,Y,lam); tot=lt+sum(L(r,T,X,Y,lam) for r in p[2])
    return math.log((len(p[2])+1)*lt/tot) if lt>0 else -20.0
def fit(X,Y,lam):
    out={}
    for i in range(len(bands)):
        ps=[p for p in P if band(p)==i]
        T=max(TE,key=lambda T:sum(g(p,T,X,Y,lam) for p in ps))
        for p in ps: out[p[0]]=g(p,T,X,Y,lam)
    return out
def frac(X,Y):
    n=s=0
    for p in P:
        for r in p[2]:
            n+=1
            if r is None: continue
            o,se=r[0],r[1]; loss=se[0]-se[o]
            if loss<=X and not (o==0 and len(se)>1 and se[0]-se[1]>Y): s+=1
    return s/n
INF=1e9
base=fit(INF,INF,0)
def row(name,X,Y,lam):
    f=fit(X,Y,lam); d=[f[k]-base[k] for k in base]
    m=st.mean(d); se=st.stdev(d)/math.sqrt(len(d))
    per=[]
    for i in range(len(bands)):
        dd=[f[p[0]]-base[p[0]] for p in P if band(p)==i]
        per.append('%+.2f'%st.mean(dd))
    print('%-34s simmed %5.1f%%  gain over static %+.3f ±%.3f  by band %s'%(name,100*frac(X,Y) if lam>0 else 0,m,1.96*se,' '.join(per)))
print('positions',len(P),'bands',bands,'static mean %.3f'%st.mean(base.values()))
row('sim every rack',INF,INF,1.0)
row('half static, half sim',INF,INF,0.5)
for X in (30,20,15,10,7,5,3):
    row('skip if static loss > %d'%X,X,INF,1.0)
for X,Y in ((15,20),(15,10),(10,10),(10,5),(7,5),(5,5)):
    row('loss > %d or best by > %d'%(X,Y),X,Y,1.0)
