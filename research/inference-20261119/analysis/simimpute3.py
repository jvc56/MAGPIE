# Imputation feature test. usage: simimpute3.py <subdir or ''> <M fit racks> [M2 ...]
# Per position (3+ unknown tiles, bag >= 7): ridge-fit sim loss on M sampled racks,
# predict held-out racks and the true rack, score picking the true rack from the held-out set.
import re,math,glob,collections,sys,statistics as st,itertools
import numpy as np
sub=sys.argv[1]; Ms=[int(x) for x in sys.argv[2:]]
D='/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/simfit/'+(sub+'/' if sub else '')+'out-*.txt'
AL='ABCDEFGHIJKLMNOPQRSTUVWXYZ?'
VAL=dict(zip(AL,[1,3,3,2,1,4,2,4,1,8,5,1,3,1,1,3,10,1,1,1,1,4,4,8,4,10,0]))
pos=collections.defaultdict(list)
for f in glob.glob(D):
    for l in open(f):
        if not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'k' not in d: continue
        if 'floor' in d: r=None
        elif 'sq' in d:
            se=[float(x) for x in d['se'].split(',')]; sq=[float(x) for x in d['sq'].split(',')]
            ns=int(d.get('ns',0)); n=len(se)-ns; o=int(d['obs']); lv=d['leave']
            r=dict(sl=se[0]-se[o],ml=max(sq[:n])-sq[o],lv=lv,klv=se[o],best=se[0],gap=(se[0]-se[1]) if n>1 else 0.0,cnt=int(d['count']),rank=int(d['rank']))
        else: continue
        pos[(d['game'],d['turn'],int(d['bag']),int(d['tiles']))].append((int(d['k']),r))
P=[]
for key,rows in pos.items():
    rows.sort(key=lambda x:x[0])
    if rows[0][0]!=0 or rows[0][1] is None or 7-key[3]<3 or key[2]<7: continue
    P.append((key,rows[0][1],[r for k,r in rows[1:]]))
K=min(len(p[2]) for p in P); P=[(k,t,s[:K]) for k,t,s in P]
def pairs(lv):
    s=sorted(lv); return set(a+b for a,b in itertools.combinations(s,2))
def build(groups,fit):
    pidx={}
    if 'pair' in groups:
        c=collections.Counter(q for r in fit for q in pairs(r['lv']))
        pidx={q:i for i,q in enumerate(sorted(q for q,n in c.items() if n>=2))}
    def feats(r):
        f=[1.0,min(r['sl'],40.0)]; pen=[1e-6,1e-3]
        if 'klv' in groups: f.append(r['klv']/10); pen.append(1)
        if 'face' in groups: f.append(sum(VAL[c] for c in r['lv'])/10); pen.append(1)
        if 'vow' in groups: f.append(sum(r['lv'].count(c) for c in 'AEIOU')); pen.append(1)
        if 'str' in groups: f+=[r['best']/20,r['gap']/10,math.log(r['cnt'])/3,min(r['rank'],20)/10]; pen+=[1,1,1,1]
        if 'let' in groups: f+=[r['lv'].count(c) for c in AL]; pen+=[10]*27
        if 'pair' in groups:
            v=[0.0]*len(pidx)
            for q in pairs(r['lv']):
                if q in pidx: v[pidx[q]]=1.0
            f+=v; pen+=[30]*len(pidx)
        return f,pen
    return feats
def predictor(p,groups,M):
    fit=[r for r in p[2][:M] if r is not None and r['sl']<=40]
    if len(fit)<8: return None
    feats=build(groups,fit)
    X=np.array([feats(r)[0] for r in fit]); y=np.array([min(r['ml'],40.0) for r in fit]); pen=np.diag(feats(fit[0])[1])
    w=np.linalg.lstsq(X.T@X+pen,X.T@y,rcond=None)[0]
    return lambda r: max(0.0,float(np.dot(w,feats(r)[0]))) if r['sl']<=40 else r['sl']
TE=(0.5,1,1.5,2,3,4,6,8,12)
def score(p,T,f,M):
    held=p[2][M:]
    L=lambda r: 0.0 if r is None else math.exp(-f(r)/T)
    lt=L(p[1]); tot=lt+sum(L(r) for r in held)
    return math.log((len(held)+1)*lt/tot) if lt>0 else -20.0
SETS=[('static loss only',()),('+ KLV',('klv',)),('+ face value',('face',)),('+ vowels',('vow',)),('+ KLV, face, vowels',('klv','face','vow')),
 ('+ rack strength',('str',)),('+ letters',('let',)),('+ letter pairs',('pair',)),('+ letters + pairs',('let','pair')),
 ('+ letters + KLV, face, vowels',('let','klv','face','vow')),('+ everything',('let','pair','klv','face','vow','str'))]
for M in Ms:
    ok=[p for p in P if predictor(p,(),M) is not None]
    print('%s | positions %d | racks per position %d | fit on %d, scored on %d + true'%(sub or 'main (500 rollouts)',len(ok),K,M,K-M))
    def run(name,fs):
        T=max(TE,key=lambda T:sum(score(p,T,f,M) for p,f in zip(ok,fs))); return [score(p,T,f,M) for p,f in zip(ok,fs)]
    base=run('static',[(lambda r:r['sl']) for p in ok])
    rows=[('actual sim on every rack',run('sim',[(lambda r:r['ml']) for p in ok]))]+[(n,run(n,[predictor(p,g,M) for p in ok])) for n,g in SETS]
    print('  static %+.3f'%st.mean(base))
    for n,v in rows:
        d=[a-b for a,b in zip(v,base)]
        print('  %-32s vs static %+.3f ±%.3f'%(n,st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d))))
