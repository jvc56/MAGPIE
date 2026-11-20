# usage: simq.py <subdir> ; compares static, sim (top 10 + observed) and sim with the same-tiles quota.
import re,math,glob,collections,sys,statistics as st
D='/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/simfit/'+sys.argv[1]+'/out-*.txt'
pos=collections.defaultdict(list); its=[]
for f in glob.glob(D):
    for l in open(f):
        if not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'k' not in d: continue
        if 'floor' in d: r=None
        elif 'sq' in d:
            se=[float(x) for x in d['se'].split(',')]; sw=[float(x) for x in d['sw'].split(',')]; sq=[float(x) for x in d['sq'].split(',')]
            if min(sw)<0: continue
            r=(int(d['obs']),se,sq,int(d['ns'])); its.append(int(d['it'])/len(se))
        else: continue
        pos[(d['game'],d['turn'],int(d['bag']))].append((int(d['k']),r))
P=[]
for key,rows in pos.items():
    rows.sort(key=lambda x:x[0])
    if len(rows)<151 or rows[0][1] is None: continue
    P.append((key,rows[0][1],[r for k,r in rows[1:151]]))
TE=(0.25,0.5,1,1.5,2,3,4,5,6,8,12,16)
def L(r,T,mode):
    if r is None: return 0.0
    o,se,sq,ns=r
    if mode=='static': return math.exp(-(se[0]-se[o])/T)
    v=sq if mode=='quota' or ns==0 else sq[:-ns]
    return math.exp((v[o]-max(v))/T)
def g(p,T,mode):
    lt=L(p[1],T,mode); tot=lt+sum(L(r,T,mode) for r in p[2])
    return math.log(151*lt/tot) if lt>0 else -20.0
bands=((46,100),(21,45),(8,20),(1,7))
def fit(mode):
    out={}; Ts=[]
    for b in bands:
        ps=[p for p in P if b[0]<=p[0][2]<=b[1]]
        T=max(TE,key=lambda T:sum(g(p,T,mode) for p in ps)); Ts.append(T)
        for p in ps: out[p[0]]=g(p,T,mode)
    return out,Ts
print(sys.argv[1],'positions',len(P),'rollouts per play (median) %.0f'%st.median(its),'| racks with extra same-tiles plays %.1f%%'%(100*sum(r is not None and r[3]>0 for p in P for r in p[2])/sum(len(p[2]) for p in P)))
F={m:fit(m) for m in ('static','sim','quota')}
def ci(d): return '%+.3f ±%.3f'%(st.mean(d),1.96*st.stdev(d)/math.sqrt(len(d)))
for m in F:
    per=[]
    for b in bands:
        per.append('%+.2f'%st.mean(F[m][0][p[0]] for p in P if b[0]<=p[0][2]<=b[1]))
    print('%-7s mean %+.3f  by band %s  best T %s'%(m,st.mean(F[m][0].values()),' '.join(per),F[m][1]))
ks=list(F['static'][0])
print('sim - static   ',ci([F['sim'][0][k]-F['static'][0][k] for k in ks]))
print('quota - sim    ',ci([F['quota'][0][k]-F['sim'][0][k] for k in ks]))
