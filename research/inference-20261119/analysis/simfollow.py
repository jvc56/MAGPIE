# Follow-up features: (A) as spreading features, (B) as a sim-free change to the static weights. usage: simfollow.py <subdir or ''> <M>
import sys,math,re,glob,statistics as st
import numpy as np
sub=sys.argv[1]; M=int(sys.argv[2])
src=open(__file__.replace('simfollow','simimpute3')).read().split("SETS=[")[0].replace("Ms=[int(x) for x in sys.argv[2:]]","Ms=[]")
# keep k and all positions (also 1-2 unknown tiles) for part B
src=src.replace("pos[(d['game'],d['turn'],int(d['bag']),int(d['tiles']))].append((int(d['k']),r))","r is not None and r.update(k=int(d['k']),g=d['game'],t=d['turn']); pos[(d['game'],d['turn'],int(d['bag']),int(d['tiles']))].append((int(d['k']),r))")
src=src.replace("or 7-key[3]<3 or key[2]<7: continue",": continue")
exec(src)
F={}
for f in glob.glob('/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/simfit/follow/out-*.txt'):
    for l in open(f):
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'fb' in d:
            b=d['fb'].split(','); a=d['fa'].split(','); F[(d['game'],d['turn'],int(d['k']))]=(d['leave'],float(b[0]),float(b[1]),float(a[0]),float(a[1]))
bad=0
for key,t,s in P:
    for r in [t]+s:
        if r is None: continue
        f=F.get((r['g'],r['t'],r['k']))
        if f is None or f[0]!=r['lv']: bad+=1; r['ds']=r['de']=r['fa']=0.0; continue
        r['ds']=f[3]-f[1]; r['de']=f[4]-f[2]; r['fa']=f[3]
print(sub or 'main (500 rollouts)','positions',len(P),'racks unmatched',bad)
# ---- B: no sims at all
TE=(1,1.5,2,3,4,6,8,12)
def sB(p,T,beta,key):
    L=lambda r: 0.0 if r is None else math.exp(-max(0.0,r['sl']-beta*r[key])/T)
    lt=L(p[1]); tot=lt+sum(L(r) for r in p[2])
    return math.log((len(p[2])+1)*lt/tot) if lt>0 else -20.0
def ci(d): return '%+.3f ±%.3f'%(st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)))
T0=max(TE,key=lambda T:sum(sB(p,T,0,'de') for p in P)); base=[sB(p,T0,0,'de') for p in P]
print('B. sim-free: credit the observed play with beta x (follow-up gain for this rack)')
print('  static, T %g: %+.3f'%(T0,st.mean(base)))
for key,name in (('de','follow-up equity gain'),('ds','follow-up score gain')):
    best=max(((st.mean(sB(p,T,b,key) for p in P),T,b) for T in TE for b in (0.1,0.2,0.3,0.5,0.75,1.0)))
    v=[sB(p,best[1],best[2],key) for p in P]
    print('  %-22s beta %.2f T %g: %s over static'%(name,best[2],best[1],ci([a-b for a,b in zip(v,base)])))
    for lo,hi in ((46,100),(21,45),(8,20),(1,7)):
        idx=[i for i,p in enumerate(P) if lo<=p[0][2]<=hi]
        print('      bag %d-%d: %s'%(lo,hi,ci([v[i]-base[i] for i in idx])))
# ---- A: spreading features (3+ unknown tiles, bag >= 7)
P=[p for p in P if 7-p[0][3]>=3 and p[0][2]>=7]
_build=build
def build2(groups,fit):
    f0=_build(groups,fit)
    def feats(r):
        f,pen=f0(r)
        if 'fol' in groups: f=f+[r['de']/10,r['ds']/10,r['fa']/10]; pen=pen+[1,1,1]
        return f,pen
    return feats
build=build2
ok=[p for p in P if predictor(p,(),M) is not None]
def run(fs):
    T=max((0.5,1,1.5,2,3,4,6,8,12),key=lambda T:sum(score(p,T,f,M) for p,f in zip(ok,fs))); return [score(p,T,f,M) for p,f in zip(ok,fs)]
base=run([(lambda r:r['sl']) for p in ok])
print('A. spreading from %d simmed racks (3+ unknown tiles, %d positions)'%(M,len(ok)))
for n,g in (('actual sim on every rack',None),('static loss only',()),('+ follow-up',('fol',)),('+ KLV, face, vowels',('klv','face','vow')),('+ KLV, face, vowels + follow-up',('klv','face','vow','fol')),('+ letters',('let',)),('+ letters + follow-up',('let','fol')),('+ everything',('let','pair','klv','face','vow','str')),('+ everything + follow-up',('let','pair','klv','face','vow','str','fol'))):
    v=run([(lambda r:r['ml']) for p in ok]) if g is None else run([predictor(p,g,M) for p in ok])
    print('  %-34s %s'%(n,ci([a-b for a,b in zip(v,base)])))
