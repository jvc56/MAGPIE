import sys,math,statistics as st
src=open(__file__.replace('simblend','simq')).read().split("print(sys.argv[1]")[0]
exec(src)
def Lb(r,T,lam):
    if r is None: return 0.0
    o,se,sq,ns=r
    n=len(se)-ns; v=[se[i]+lam*(sq[i]-se[i]) for i in range(n)]
    return math.exp((v[o]-max(v))/T)
def gb(p,T,lam):
    lt=Lb(p[1],T,lam); tot=lt+sum(Lb(r,T,lam) for r in p[2])
    return math.log(151*lt/tot) if lt>0 else -20.0
base=None
for lam in (0,0.1,0.2,0.3,0.5,0.75,1):
    out={}; Ts=[]
    for b in bands:
        ps=[p for p in P if b[0]<=p[0][2]<=b[1]]
        T=max(TE,key=lambda T:sum(gb(p,T,lam) for p in ps)); Ts.append(T)
        for p in ps: out[p[0]]=gb(p,T,lam)
    if lam==0: base=out
    d=[out[k]-base[k] for k in out]
    per=['%+.2f'%st.mean(out[p[0]] for p in P if b[0]<=p[0][2]<=b[1]) for b in bands]
    print('sim weight %.2f: mean %+.3f  vs static %+.3f ±%.3f  by band %s  T %s'%(lam,st.mean(out.values()),st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)),' '.join(per),Ts))
