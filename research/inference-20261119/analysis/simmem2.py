# Carry-over test (vectorised). Weight of a candidate rack = current-turn likelihood x previous turn's lift
# averaged over which tiles were held over.
import re,math,glob,collections,statistics as st
import numpy as np
B='/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/simfit/'
AL='ABCDEFGHIJKLMNOPQRSTUVWXYZ?'
racks={}
for l in open(__file__.replace('simmem2.py','tfit-prod.in')):
    g,t,rk,mv=l.split(); racks[(g,t)]=rk
sim={}
for f in glob.glob(B+'out-*.txt'):
    for l in open(f):
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'sq' in d:
            sq=[float(x) for x in d['sq'].split(',')]; sim[(d['game'],d['turn'],d['k'])]=(d['leave'],max(sq)-sq[int(d['obs'])])
pos=collections.defaultdict(dict)
for f in glob.glob(B+'mem/out-*.txt'):
    for l in open(f):
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'k' in d: pos[(d['game'],d['turn'],int(d['bag']),int(d['tiles']))][int(d['k'])]=d
P=[]
for key,rows in pos.items():
    t=rows.get(0)
    if t is None or 'mem' not in t or 'se' not in t or len(rows)<151: continue
    used=collections.Counter(racks[(key[0],key[1])]); used.subtract(collections.Counter(t['leave']))
    need=collections.Counter(t['pl'])
    n=151; ok=np.zeros(n,bool); sl=np.zeros(n); ml=np.full(n,np.nan); mem=np.ones((n,5)); has=np.zeros(n); F=np.zeros((n,30))
    for k in range(n):
        d=rows[k]
        if 'se' not in d: continue
        se=[float(x) for x in d['se'].split(',')]; o=int(d['obs']); lv=d['leave']
        ok[k]=True; sl[k]=se[0]-se[o]; mem[k]=[float(x) for x in d['mem'].split(',')]
        sm=sim.get((key[0],key[1],str(k)))
        if sm and sm[0]==lv: ml[k]=sm[1]
        full=collections.Counter(lv)+used; has[k]=all(full[c]>=m for c,m in need.items())
        F[k,0]=se[o]; F[k,1]=lv.count('?'); F[k,2]=lv.count('S'); F[k,3:]=[lv.count(c) for c in AL]
    P.append(dict(key=key,ok=ok,sl=sl,ml=ml,mem=mem,has=has,F=F,kept=len(t['pl']),simok=bool(np.all(~np.isnan(ml[ok])))))
print('positions with a previous-turn leave to carry over:',len(P),'| mean tiles kept last turn %.1f'%st.mean(p['kept'] for p in P))
GRID=(2,3,4,6,8); TE=(1,1.5,2,3,4,6,8,12)
def ev(p,m,T,g,gam):
    if m=='prior': w=p['ok'].astype(float)
    else:
        w=np.where(p['ok'],np.exp(-np.nan_to_num(p['ml'] if m.startswith('sim') else p['sl'])/T),0.0)
        if m.endswith('+mem'): w=w*p['mem'][:,g]**gam
        if m.endswith('+oracle'): w=w*p['has']
    nat=math.log(151*w[0]/w.sum()) if w[0]>0 else -20.0
    h=w[1:]; s=h.sum()
    if s<=0: h=p['ok'][1:].astype(float); s=h.sum()
    e=(h@p['F'][1:]/s-p['F'][0])**2
    return nat,(e[0],e[1:3].sum(),e[3:].sum())
def best(ps,m):
    opts=[(1,0,1)] if m=='prior' else [(T,g,gam) for T in TE for g in (range(5) if m.endswith('+mem') else [0]) for gam in ((0.5,1.0) if m.endswith('+mem') else [1.0])]
    o=max(opts,key=lambda o:sum(ev(p,m,*o)[0] for p in ps)); return [ev(p,m,*o) for p in ps],o
def ci(d): return '%+.3f ±%.3f'%(st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)))
def report(name,ps,models):
    if len(ps)<20: return
    print('==',name,'| n',len(ps))
    R={m:best(ps,m) for m in ['prior']+models}; pr=R['prior'][0]
    for m in models:
        v,o=R[m]; ref=R[m.split('+')[0]][0]
        red=['%5.1f%%'%(100*(1-st.mean(x[1][i] for x in v)/st.mean(x[1][i] for x in pr))) for i in range(3)]
        extra='' if '+' not in m else ' | vs %s %s'%(m.split('+')[0],ci([a[0]-b[0] for a,b in zip(v,ref)]))
        par='' if not m.endswith('+mem') else ' (prev T %g, power %g)'%(GRID[o[1]],o[2])
        print('  %-14s nats %+.3f | error removed: KLV %s  blank+S %s  letters %s%s%s'%(m,st.mean(x[0] for x in v),red[0],red[1],red[2],extra,par))
report('all',P,['static','static+mem','static+oracle'])
for lo,hi in ((46,100),(21,45),(8,20),(1,7)):
    report('bag %d-%d'%(lo,hi),[p for p in P if lo<=p['key'][2]<=hi],['static','static+mem'])
for lo,hi,nm in ((1,2,'kept 1-2 tiles last turn'),(3,4,'kept 3-4'),(5,6,'kept 5-6 (played 1-2 tiles last turn)')):
    report(nm,[p for p in P if lo<=p['kept']<=hi],['static','static+mem','static+oracle'])
report('positions with 500-rollout sims on every rack',[p for p in P if p['simok']],['static','static+mem','sim','sim+mem'])
