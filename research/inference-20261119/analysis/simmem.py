# Carry-over test: multiply each candidate rack's weight by the previous turn's lift (averaged over which tiles were held over).
import re,math,glob,collections,statistics as st
import numpy as np
B='/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/simfit/'
AL='ABCDEFGHIJKLMNOPQRSTUVWXYZ?'
racks={}
for l in open(__file__.replace('simmem.py','tfit-prod.in')):
    g,t,rk,mv=l.split(); racks[(g,t)]=rk
sim={}
for f in glob.glob(B+'out-*.txt'):
    for l in open(f):
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'sq' in d:
            sq=[float(x) for x in d['sq'].split(',')]; sim[(d['game'],d['turn'],d['k'])]=(d['leave'],max(sq)-sq[int(d['obs'])])
pos=collections.defaultdict(list)
for f in glob.glob(B+'mem/out-*.txt'):
    for l in open(f):
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'k' not in d: continue
        if 'floor' in d or 'se' not in d: r=None
        else:
            se=[float(x) for x in d['se'].split(',')]; o=int(d['obs'])
            sm=sim.get((d['game'],d['turn'],d['k']))
            r=dict(sl=se[0]-se[o],lv=d['leave'],klv=se[o],mem=[float(x) for x in d['mem'].split(',')] if 'mem' in d else None,pl=d.get('pl'),ml=sm[1] if sm and sm[0]==d['leave'] else None)
        pos[(d['game'],d['turn'],int(d['bag']),int(d['tiles']))].append((int(d['k']),r))
def sub(a,b):
    c=collections.Counter(a); c.subtract(collections.Counter(b)); return c
P=[]
for key,rows in pos.items():
    rows.sort(key=lambda x:x[0])
    if len(rows)<151 or rows[0][1] is None or rows[0][1]['mem'] is None: continue
    t=rows[0][1]; used=sub(racks[(key[0],key[1])],t['lv'])
    for k,r in rows:
        if r is None: continue
        full=collections.Counter(r['lv'])+used; need=collections.Counter(t['pl'])
        r['has']=all(full[c]>=n for c,n in need.items())
    P.append((key,t,[r for k,r in rows[1:151]]))
print('positions with a previous-turn leave to carry over:',len(P),'| mean tiles kept last turn %.1f'%st.mean(len(p[1]['pl']) for p in P))
GRID=(2,3,4,6,8); TE=(1,1.5,2,3,4,6,8,12)
def W(r,m,T,g,gam):
    if r is None: return 0.0
    if m=='prior': return 1.0
    base=math.exp(-(r['ml'] if m.startswith('sim') else r['sl'])/T)
    if m.endswith('+mem'): return base*r['mem'][g]**gam
    if m.endswith('+oracle'): return base*(1.0 if r['has'] else 0.0)
    return base
def feats(r): return np.array([r['klv']]),np.array([r['lv'].count('?'),r['lv'].count('S')],float),np.array([r['lv'].count(c) for c in AL],float)
def ev(p,m,T,g,gam):
    lt=W(p[1],m,T,g,gam); ws=[W(r,m,T,g,gam) for r in p[2]]; tot=lt+sum(ws)
    nat=math.log(151*lt/tot) if lt>0 else -20.0
    held=[(w,r) for w,r in zip(ws,p[2]) if r is not None]; s=sum(w for w,_ in held)
    if s<=0: held=[(1.0,r) for _,r in held]; s=len(held)
    tf=feats(p[1]); e=[]
    for i in range(3):
        e.append(float(((sum(w*feats(r)[i] for w,r in held)/s-tf[i])**2).sum()))
    return nat,e
def best(ps,m):
    opts=[(T,g,gam) for T in TE for g in (range(len(GRID)) if m.endswith('+mem') else [0]) for gam in ((0.5,1.0) if m.endswith('+mem') else [1.0])]
    if m=='prior': opts=[(1,0,1)]
    o=max(opts,key=lambda o:sum(ev(p,m,*o)[0] for p in ps)); return [ev(p,m,*o) for p in ps],o
def ci(d): return '%+.3f ±%.3f'%(st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)))
def report(name,ps,models):
    print('==',name,'n',len(ps))
    R={m:best(ps,m) for m in ['prior']+models}
    pr=R['prior'][0]
    for m in models:
        v,o=R[m]; ref=R[m.split('+')[0]][0]
        red=['%5.1f%%'%(100*(1-st.mean(x[1][i] for x in v)/st.mean(x[1][i] for x in pr))) for i in range(3)]
        extra='' if '+' not in m else ' | vs %s %s'%(m.split('+')[0],ci([a[0]-b[0] for a,b in zip(v,ref)]))
        par='' if not m.endswith('+mem') else ' (prev T %g, power %g)'%(GRID[o[1]],o[2])
        print('  %-14s nats %+.3f | error removed: KLV %s blank+S %s letters %s%s%s'%(m,st.mean(x[0] for x in v),red[0],red[1],red[2],extra,par))
report('all',P,['static','static+mem','static+oracle'])
for lo,hi in ((46,100),(21,45),(8,20),(1,7)):
    report('bag %d-%d'%(lo,hi),[p for p in P if lo<=p[0][2]<=hi],['static','static+mem'])
PS=[p for p in P if p[1]['ml'] is not None and all(r is None or r['ml'] is not None for r in p[2])]
report('positions with 500-rollout sims on every rack',PS,['static','static+mem','sim','sim+mem'])
