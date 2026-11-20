# Stratified pilot test: choose N racks to sim out of a 500-rack pool by different rules, fit the letters
# model on them, score on 100 held-out prior racks + the true rack. usage: simstrat.py <subdir> <N> [N2...]
import sys,math,random,statistics as st
import numpy as np
sub=sys.argv[1]; Ns=[int(x) for x in sys.argv[2:]]
src=open(__file__.replace('simstrat','simimpute3')).read().split("SETS=[")[0].replace("Ms=[int(x) for x in sys.argv[2:]]","Ms=[]")
exec(src)
HELD=100; POOL=K-HELD; REPS=3
def fitpred(fit,groups=('let',)):
    fit=[r for r in fit if r is not None and r['sl']<=40]
    if len(fit)<8: return lambda r:r['sl']
    feats=build(groups,fit)
    X=np.array([feats(r)[0] for r in fit]); y=np.array([min(r['ml'],40.0) for r in fit]); pen=np.diag(feats(fit[0])[1])
    w=np.linalg.lstsq(X.T@X+pen,X.T@y,rcond=None)[0]
    return lambda r: max(0.0,float(np.dot(w,feats(r)[0]))) if r['sl']<=40 else r['sl']
def wsample(rng,items,weights,n):
    # weighted sampling without replacement (Efraimidis-Spirakis)
    keyed=sorted(((rng.random()**(1.0/max(w,1e-12)),i) for i,w in enumerate(weights)),reverse=True)
    return [items[i] for _,i in keyed[:n]]
def distinct(pool):
    seen=set(); out=[]
    for r in pool:
        if r is not None and r['lv'] not in seen: seen.add(r['lv']); out.append(r)
    return out
def pick(rule,pool,n,rng):
    pool=[r for r in pool if r is not None]
    if rule=='random': return rng.sample(pool,min(n,len(pool)))
    d=distinct(pool)
    if rule=='distinct': return rng.sample(d,min(n,len(d)))
    if rule.startswith('static T'):
        T=float(rule.split('T')[1]); return wsample(rng,d,[math.exp(-r['sl']/T) for r in d],n)
    if rule.startswith('loss <='):
        X=float(rule.split('<=')[1]); near=[r for r in d if r['sl']<=X]; rest=[r for r in d if r['sl']>X]
        got=rng.sample(near,min(n,len(near))); return got+rng.sample(rest,min(n-len(got),len(rest)))
    if rule=='banded':
        a=[r for r in d if r['sl']<=10]; b=[r for r in d if 10<r['sl']<=30]; c=[r for r in d if r['sl']>30]
        got=rng.sample(a,min(int(n*0.6),len(a))); got+=rng.sample(b,min(int(n*0.35),len(b)))
        left=[r for r in d if r not in got]; return got+rng.sample(left,min(n-len(got),len(left)))
    if rule.startswith('letters'):
        # greedy: favour racks holding letters seen least so far; optionally among statically plausible racks
        cand=[r for r in d if r['sl']<=20] if 'near' in rule else list(d)
        rng.shuffle(cand); cnt={c:0 for c in AL}; got=[]
        while cand and len(got)<n:
            j=max(range(len(cand)),key=lambda i:sum(1.0/(1+cnt[c]) for c in set(cand[i]['lv'])))
            r=cand.pop(j); got.append(r)
            for c in r['lv']: cnt[c]+=1
        left=[r for r in d if r not in got]; return got+rng.sample(left,min(n-len(got),len(left)))
def shape_err(p,f,T):
    held=[r for r in p[2][POOL:] if r is not None]
    w=np.array([math.exp(-f(r)/T) for r in held]); w=w/w.sum() if w.sum()>0 else np.ones(len(held))/len(held)
    t=p[1]; e={}
    e['klv']=(sum(wi*r['klv'] for wi,r in zip(w,held))-t['klv'])**2
    e['power']=sum((sum(wi*r['lv'].count(c) for wi,r in zip(w,held))-t['lv'].count(c))**2 for c in '?S')
    return e
def nats(p,f,T):
    held=p[2][POOL:]; L=lambda r: 0.0 if r is None else math.exp(-f(r)/T)
    lt=L(p[1]); tot=lt+sum(L(r) for r in held)
    return math.log((len(held)+1)*lt/tot) if lt>0 else -20.0
TE=(0.5,1,1.5,2,3,4,6,8,12)
def evaluate(fs):
    T=max(TE,key=lambda T:sum(nats(p,f,T) for p,f in zip(P,fs)))
    return [nats(p,f,T) for p,f in zip(P,fs)],[shape_err(p,f,T) for p,f in zip(P,fs)]
def ci(d): return '%+.3f ±%.3f'%(st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)))
bn,bs=evaluate([(lambda r:r['sl']) for p in P])
print(sub,'positions',len(P),'| pool',POOL,'held-out',HELD,'| static nats %+.3f'%st.mean(bn))
RULES=('random','distinct','static T8','static T4','loss <=20','loss <=10','banded','letters','letters near')
for N in Ns:
    print('simmed racks per position:',N,'   (nats vs static | vs random rule | KLV error vs static | blank+S error vs static)')
    ref=None
    for rule in RULES:
        dn=[0.0]*len(P); dk=[0.0]*len(P); dp=[0.0]*len(P)
        for rep in range(REPS):
            rng=random.Random(1000*rep+N)
            n,s=evaluate([fitpred(pick(rule,p[2][:POOL],N,rng)) for p in P])
            for i in range(len(P)):
                dn[i]+=(n[i]-bn[i])/REPS; dk[i]+=(s[i]['klv']-bs[i]['klv'])/REPS; dp[i]+=(s[i]['power']-bs[i]['power'])/REPS
        if ref is None: ref=dn
        mk=st.mean(b['klv'] for b in bs); mp=st.mean(b['power'] for b in bs)
        print('   %-14s %s | %s | %+5.1f%% | %+5.1f%%'%(rule,ci(dn),ci([a-b for a,b in zip(dn,ref)]) if rule!='random' else '      -       ',100*st.mean(dk)/mk,100*st.mean(dp)/mp))
