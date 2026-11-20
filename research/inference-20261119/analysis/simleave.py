import math,statistics as st,sys
sys.argv=[sys.argv[0]]
exec(open(__file__.replace('simleave','simfit')).read().split("bands=")[0])
TE=(0.5,1,1.5,2,3,4,6,8,12)
def L(r,T,sim):
    if r is None: return 0.0
    v=r[3] if sim else r[1]; return math.exp((v[r[0]]-max(v))/T)
def g(p,T,sim):
    lt=L(p[1],T,sim); tot=lt+sum(L(r,T,sim) for r in p[2])
    return math.log((len(p[2])+1)*lt/tot) if lt>0 else -20.0
for n in range(1,7):
    ps=[p for p in P if 7-p[0][3]==n and p[0][2]>=7]
    if len(ps)<10: continue
    Ts=max(TE,key=lambda T:sum(g(p,T,False) for p in ps)); Tm=max(TE,key=lambda T:sum(g(p,T,True) for p in ps))
    a=[g(p,Ts,False) for p in ps]; d=[g(p,Tm,True)-x for p,x in zip(ps,a)]
    distinct=st.mean(len(set(id(r) for r in p[2])) for p in ps)
    print('leave %d: n %4d | static %+.2f | sim gain %+.2f ±%.2f'%(n,len(ps),st.mean(a),st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d))))
