import math,random,collections
f='data/benchmarks/softmax-20261115/decide-fixed100k-t8/run.log'
sim={}; arm={}; done=set(); bag={}
for l in open(f):
    if l.startswith('INFDEC_GAME'): done.add(int(l.split()[2].split('=')[1]))
    elif l.startswith('INFDEC_SIM'):
        p=l.split(); g=int(p[2][5:]); t=int(p[3][5:]); a=p[4][4:]
        pl=[]
        for x in p[6:]:
            c=x.split(',')
            pl.append((c[0],float(c[3]),float(c[4]),int(c[1])))
        sim[(g,t,a)]=pl
    elif l.startswith('INFDEC_ARM'):
        kv=dict(x.split('=',1) for x in l.split()[1:])
        arm[(int(kv['game']),int(kv['turn']))]=kv
keys=[k for k in arm if k[0] in done]
n=len(keys); G=len(done)
print('decisions',n,'games',G)
Phi=lambda z:0.5*(1+math.erf(z/math.sqrt(2)))
random.seed(1)
# 1. expected selection loss of each arm in its own model, at k x budget (predictive, flat prior)
def loss(pl,k,R=40):
    tot=0
    for _ in range(R):
        mu=[random.gauss(m,s) for _,m,s,_ in pl]
        if k is None: pick=0
        elif k==0: pick=max(range(len(pl)),key=lambda i:pl[i][1])
        else:
            est=[random.gauss(mu[i],pl[i][2]/math.sqrt(k)) for i in range(len(pl))]
            pick=max(range(len(pl)),key=lambda i:est[i])
        tot+=max(mu)-mu[pick]
    return tot/R
sub=random.sample(keys,6000)
for k in [0,1,4,16,64,1e9]:
    lc=ls=0;m=0
    for g,t in sub:
        if (g,t,'0') in sim and (g,t,'s') in sim:
            lc+=loss(sim[(g,t,'0')],k); ls+=loss(sim[(g,t,'s')],k); m+=1
    print('budget x%-6g selection loss, utility pp: control %.4f  T8 %.4f  diff %.4f (n=%d)'%(k,100*lc/m,100*ls/m,100*(ls-lc)/m,m))
# 2. gain split by how decisive each arm was about its own choice over the other arm's move
rows=[]
miss=0
for g,t in keys:
    kv=arm[(g,t)]
    if kv['same']=='1': continue
    A=kv['ctl_move']; B=kv['move']
    def z(pl,x,y):
        d={p[0]:p for p in pl}
        if x not in d: return None
        if y not in d: return 9.0
        return (d[x][1]-d[y][1])/math.hypot(d[x][2],d[y][2])
    zc=z(sim.get((g,t,'0'),[]),A,B); zs=z(sim.get((g,t,'s'),[]),B,A)
    if zc is None or zs is None: miss+=1; continue
    rows.append((g,zc,zs,float(kv['sim_dwin']),float(kv['sim_dutility']),float(kv['sim_dequity'])))
print('differing',len(rows),'unparsed',miss)
def clus(sel,idx,w=None):
    by=collections.defaultdict(float)
    for i,r in enumerate(sel): by[r[0]]+=r[idx]*(w[i] if w else 1)
    m=sum(by.values())/n
    se=math.sqrt(G/(G-1)*sum(v*v for v in by.values()))/n
    return m,1.96*se
def show(name,sel,w=None):
    s=name+' | n %d'%len(sel)
    for idx,sc in ((3,100),(4,100),(5,1)):
        m,ci=clus(sel,idx,w); s+=' | %+.4f ±%.4f'%(m*sc,ci*sc)
    print(s)
print('contribution to per-decision gain (win pp | utility pp | equity), denominators = all decisions')
show('all differing',rows)
for lo,hi,nm in ((-99,1,'weak: min z <1'),(1,2,'min z 1-2'),(2,3,'min z 2-3'),(3,99,'decisive: min z >=3')):
    show(nm,[r for r in rows if lo<=min(r[1],r[2])<hi])
show('control decisive (zc>=2)',[r for r in rows if r[1]>=2])
show('control not decisive (zc<2)',[r for r in rows if r[1]<2])
show('T8 decisive (zs>=2)',[r for r in rows if r[2]>=2])
show('T8 not decisive (zs<2)',[r for r in rows if r[2]<2])
# infinite-budget weight: both arms keep their choice
w=[Phi(r[1])+Phi(r[2])-1 for r in rows]
show('weighted by P(control keeps)+P(T8 keeps)-1',rows,w)
