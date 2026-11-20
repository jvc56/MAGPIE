import math,re,statistics as st,sys
sys.argv=[sys.argv[0]]
exec(open(__file__.replace('simtie','simfit')).read().split("bands=")[0])
G={}
for l in open(__file__.replace('simtie.py','sgap.txt')):
    d=dict(re.findall(r'(\w+)=(\S+)',l)); G[(d['game'],d['turn'])]=d
def cls(d):
    if float(d['gap'])>0: return 'A better same-tiles play exists'
    if int(d['ties'])>0: return 'B exact tie, none better'
    if int(d['within2'])>0: return 'C observed is best by <= 2'
    if int(d['within5'])>0: return 'D best by 2 to 5'
    if int(d['others'])>0: return 'E best by > 5'
    return 'F only placement for these tiles'
names=sorted(set(cls(d) for d in G.values()))
tot=len(G)
TE=(0.5,1,1.5,2,3,4,6,8,12)
def L(r,T,sim):
    if r is None: return 0.0
    v=r[3] if sim else r[1]
    return math.exp((v[r[0]]-max(v))/T)
def g(p,T,sim):
    lt=L(p[1],T,sim); tot=lt+sum(L(r,T,sim) for r in p[2])
    return math.log((len(p[2])+1)*lt/tot) if lt>0 else -20.0
Ts=max(TE,key=lambda T:sum(g(p,T,False) for p in P)); Tm=max(TE,key=lambda T:sum(g(p,T,True) for p in P))
print('positions with sim data',len(P),'static T',Ts,'sim T',Tm)
allgain=sum(g(p,Tm,True)-g(p,Ts,False) for p in P)
for n in names:
    share=100*sum(cls(d)==n for d in G.values())/tot
    ps=[p for p in P if cls(G[(p[0][0],p[0][1])])==n]
    if len(ps)<3: print('%-34s %5.1f%% of placements | n %d'%(n,share,len(ps))); continue
    a=[g(p,Ts,False) for p in ps]; d=[g(p,Tm,True)-x for p,x in zip(ps,a)]
    print('%-34s %5.1f%% of placements | n %3d | static %+.2f | sim gain %+.2f ±%.2f | share of total sim gain %4.0f%%'%(n,share,len(ps),st.mean(a),st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)),100*sum(d)/allgain))
