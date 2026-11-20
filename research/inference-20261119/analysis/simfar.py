import math,statistics as st,sys
sys.argv=[sys.argv[0]]
exec(open(__file__.replace('simfar','simfit')).read().split("bands=")[0])
def loss(r): return 1e9 if r is None else r[1][0]-r[1][r[0]]
tl=[loss(p[1]) for p in P]; sl=[loss(r) for p in P for r in p[2]]
print('positions',len(P))
for X in (5,10,15,20,30,50):
    # of true racks beyond X, how often does the mini-sim put the observed play within 2 points of its best?
    far=[p[1] for p in P if loss(p[1])>X]
    ok=sum(max(r[3])-r[3][r[0]]<=2 for r in far)
    print('static loss > %2d: sampled racks %5.1f%%  true racks %5.1f%% (%d), of which sim says fine: %d'%(X,100*sum(x>X for x in sl)/len(sl),100*sum(x>X for x in tl)/len(tl),len(far),ok))
# two-temperature pruning: skipped racks use static weight at Ts, simmed racks sim weight at Tm
TE=(0.5,1,1.5,2,3,4,6,8,12)
def L(r,X,Ts,Tm):
    if r is None: return 0.0
    o,se,sw,sq=r
    if se[0]-se[o]>X: return math.exp(-(se[0]-se[o])/Ts)
    return math.exp((sq[o]-max(sq))/Tm)
def g(p,X,Ts,Tm):
    lt=L(p[1],X,Ts,Tm); tot=lt+sum(L(r,X,Ts,Tm) for r in p[2])
    return math.log((len(p[2])+1)*lt/tot) if lt>0 else -20.0
base=max(st.mean(g(p,-1,T,1) for p in P) for T in TE)
print('static only, one temperature: %+.3f'%base)
for X in (1e9,30,20,15,10,5):
    best=max((st.mean(g(p,X,Ts,Tm) for p in P),Ts,Tm) for Ts in TE for Tm in TE)
    print('skip if loss > %-5g simmed %5.1f%%  gain %+.3f (static T %g, sim T %g)  over static %+.3f'%(X,100*sum(x<=X for x in sl)/len(sl),best[0],best[1],best[2],best[0]-base))
