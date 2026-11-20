import math,re,statistics as st,sys
sys.argv=[sys.argv[0]]
exec(open(__file__.replace('simgap','simfit')).read().split("bands=")[0])
G={}
for l in open(__file__.replace('simgap.py','sgap.txt')):
    d=dict(re.findall(r'(\w+)=(\S+)',l)); G[(d['game'],d['turn'])]=(float(d['gap']),int(d['scoregap']),int(d['bag']))
print('all placements',len(G))
for lo,hi in ((1,100),(46,100),(21,45),(8,20),(1,7)):
    v=[g for g in G.values() if lo<=g[2]<=hi]
    print('bag %d-%d n %d: equity gap >0 %.1f%%  >2 %.1f%%  >5 %.1f%%  >10 %.1f%% | higher-scoring same-tiles play %.1f%%'%(lo,hi,len(v),*[100*sum(g[0]>x for g in v)/len(v) for x in (0,2,5,10)],100*sum(g[1]>0 for g in v)/len(v)))
TE=(0.5,1,1.5,2,3,4,6,8,12)
def gap(p): return G[(p[0][0],p[0][1])][0]
def L(r,T,off,sim):
    if r is None: return 0.0
    o,se,sw,sq=r
    if sim: return math.exp((sq[o]-max(sq))/T)
    return math.exp(-max(0.0,se[0]-se[o]-off)/T)
def g(p,T,corr,sim=False):
    off=gap(p) if corr else 0.0
    lt=L(p[1],T,off,sim); tot=lt+sum(L(r,T,off,sim) for r in p[2])
    return math.log((len(p[2])+1)*lt/tot) if lt>0 else -20.0
def best(ps,corr,sim=False):
    T=max(TE,key=lambda T:sum(g(p,T,corr,sim) for p in ps)); return [g(p,T,corr,sim) for p in ps],T
def rep(name,ps):
    if len(ps)<5: print(name,'n',len(ps)); return
    a,Ta=best(ps,False); b,Tb=best(ps,True); c,Tc=best(ps,False,True)
    d=[y-x for x,y in zip(a,b)]; e=[y-x for x,y in zip(a,c)]
    print('%-28s n %4d | static %+.2f (T%g) | gap-corrected %+.2f (T%g), diff %+.3f ±%.3f | sim %+.2f, diff %+.3f ±%.3f'%(name,len(ps),st.mean(a),Ta,st.mean(b),Tb,st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)),st.mean(c),st.mean(e),1.96*st.stdev(e)/math.sqrt(len(e))))
print('positions with sim data',len(P))
rep('all',P); rep('gap = 0',[p for p in P if gap(p)==0]); rep('gap > 0',[p for p in P if gap(p)>0]); rep('gap > 5',[p for p in P if gap(p)>5])
