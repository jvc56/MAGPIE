# Imputation test. Per position: fit sim loss on M sampled racks, predict it
# for the other sampled racks and the true rack from static loss + letters,
# and score how well the true rack is picked out of the held-out set.
# usage: simimpute.py <glob-subdir or ''> <M>
import re,math,glob,collections,sys,statistics as st
import numpy as np
sub=sys.argv[1]; M=int(sys.argv[2])
D='/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/simfit/'+(sub+'/' if sub else '')+'out-*.txt'
AL='ABCDEFGHIJKLMNOPQRSTUVWXYZ?'
pos=collections.defaultdict(list)
for f in glob.glob(D):
    for l in open(f):
        if not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'k' not in d: continue
        if 'floor' in d: r=None
        elif 'sq' in d:
            se=[float(x) for x in d['se'].split(',')]; sq=[float(x) for x in d['sq'].split(',')]
            ns=int(d.get('ns',0)); n=len(se)-ns; o=int(d['obs'])
            lv=d['leave']; r=(se[0]-se[o], max(sq[:n])-sq[o], [lv.count(c) for c in AL], [se[0]/20.0, (se[0]-se[1])/10.0 if n>1 else 0.0, math.log(int(d['count']))/3.0, min(int(d['rank']),20)/10.0], [sum(lv.count(c) for c in 'AEIOU'), len(lv)-len(set(lv)), sum(lv.count(c) for c in 'JQXZKVW')])
        else: continue
        pos[(d['game'],d['turn'],int(d['bag']),int(d['tiles']))].append((int(d['k']),r))
P=[]
for key,rows in pos.items():
    rows.sort(key=lambda x:x[0])
    if len(rows)<151 or rows[0][1] is None or 7-key[3]<3 or key[2]<7: continue
    P.append((key,rows[0][1],[r for k,r in rows[1:151]]))
def feats(r,letters):
    f=[1.0,min(r[0],40.0)]
    if letters in (True,'L','LS','LG','all'): f+=r[2]
    if letters in ('S','LS','all'): f+=r[3]
    if letters in ('G','LG','all'): f+=r[4]
    return f
def predict(p,letters,lam):
    fit=[r for r in p[2][:M] if r is not None and r[0]<=40]
    if len(fit)<8: return None
    X=np.array([feats(r,letters) for r in fit]); y=np.array([min(r[1],40.0) for r in fit])
    pen=lam*np.eye(X.shape[1]); pen[0,0]=1e-6; pen[1,1]=1e-3
    w=np.linalg.lstsq(X.T@X+pen,X.T@y,rcond=None)[0]
    return lambda r: max(0.0,float(np.dot(w,feats(r,letters)))) if r[0]<=40 else r[0]
TE=(0.5,1,1.5,2,3,4,6,8,12)
def score(p,T,f):
    held=p[2][M:]
    def L(r):
        return 0.0 if r is None else math.exp(-f(r)/T)
    lt=L(p[1]); tot=lt+sum(L(r) for r in held)
    return math.log((len(held)+1)*lt/tot) if lt>0 else -20.0
models={'static':lambda p:(lambda r:r[0]),'actual sim on every rack':lambda p:(lambda r:r[1]),
 'static loss only':lambda p:predict(p,False,0),
 '+ rack strength (4 static feats)':lambda p:predict(p,'S',10.0),
 '+ leave shape (vowels,dups,heavy)':lambda p:predict(p,'G',10.0),
 '+ letters':lambda p:predict(p,'L',10.0),
 '+ letters + rack strength':lambda p:predict(p,'LS',10.0),
 '+ letters + leave shape':lambda p:predict(p,'LG',10.0),
 '+ all':lambda p:predict(p,'all',10.0)}
ok=[p for p in P if predict(p,False,0) is not None]
print(sub or 'main (500 rollouts)','positions with 3+ unknown tiles',len(ok),'| fit on',M,'racks, scored on',150-M,'+ true')
res={}
for name,mk in models.items():
    fs=[mk(p) for p in ok]
    T=max(TE,key=lambda T:sum(score(p,T,f) for p,f in zip(ok,fs)))
    res[name]=[score(p,T,f) for p,f in zip(ok,fs)]
    d=[a-b for a,b in zip(res[name],res['static'])]
    print('%-36s %+.3f (T %g)  vs static %+.3f ±%.3f'%(name,st.mean(res[name]),T,st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)) if name!='static' else 0))
