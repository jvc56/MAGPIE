# Leave-shape error: posterior-mean features (from weighted held-out sampled racks) vs the true leave's features.
# usage: simshape.py <subdir or ''> <held> <M1> [M2...]
import sys,math,statistics as st
import numpy as np
sub=sys.argv[1]; HELD=int(sys.argv[2]); Mlist=[int(x) for x in sys.argv[3:]]
src=open(__file__.replace('simshape','simimpute3')).read().split("SETS=[")[0].replace("Ms=[int(x) for x in sys.argv[2:]]","Ms=[]")
exec(src)
def shape(r):
    lv=r['lv']
    return dict(letters=np.array([lv.count(c) for c in AL],float),klv=np.array([r['klv']]),vowels=np.array([sum(lv.count(c) for c in 'AEIOU')],float),
                face=np.array([sum(VAL[c] for c in lv)],float),power=np.array([lv.count('?'),lv.count('S')],float))
KEYS=('letters','power','vowels','klv','face')
def errs(p,f,T):
    held=[r for r in p[2][len(p[2])-HELD:] if r is not None]
    w=np.array([1.0 if f is None else math.exp(-f(r)/T) for r in held])
    if w.sum()<=0: w=np.ones(len(held))
    w/=w.sum(); t=shape(p[1]); sh=[shape(r) for r in held]
    return {k:float(((sum(wi*s[k] for wi,s in zip(w,sh))-t[k])**2).sum()) for k in KEYS}
TE=(0.5,1,1.5,2,3,4,6,8,12)
ok=[p for p in P if predictor(p,(),min(Mlist)) is not None]
def run(fs):
    if fs[0] is None: return [errs(p,None,1) for p in ok]
    T=min(TE,key=lambda T:sum(errs(p,f,T)['letters'] for p,f in zip(ok,fs))); return [errs(p,f,T) for p,f in zip(ok,fs)]
prior=run([None]*len(ok))
rows=[('static',run([(lambda r:r['sl']) for p in ok]))]
for M in Mlist:
    rows.append(('imputed from %d racks'%M,run([predictor(p,('let','pair') if M>=300 else ('let',),M) for p in ok])))
rows.append(('actual sim on every rack',run([(lambda r:r['ml']) for p in ok])))
print(sub or 'main (500 rollouts)','positions',len(ok),'| posterior from',HELD,'held-out racks | % of the prior\'s squared error removed (higher is better)')
print('%-26s'%''+''.join('%16s'%k for k in KEYS))
print('%-26s'%'prior MSE'+''.join('%16.2f'%st.mean(e[k] for e in prior) for k in KEYS))
for name,es in rows:
    out=[]
    for k in KEYS:
        d=[a[k]-b[k] for a,b in zip(prior,es)]; m=st.mean(a[k] for a in prior)
        out.append('%5.1f%% ±%.1f'%(100*st.mean(d)/m,100*1.96*st.stdev(d)/math.sqrt(len(d))/m))
    print('%-26s'%name+''.join('%16s'%o for o in out))
