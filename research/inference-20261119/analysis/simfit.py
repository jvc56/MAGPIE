# Fits likelihood models to the infsfit replay. For each position, gain =
# ln L(true leave) - ln mean L(prior-sampled leaves): nats gained over the
# prior in predicting the kept tiles. Reports the mean gain per bag band at
# each model's best temperature.
import re,math,glob,collections,sys
D='/Users/john/sources/nov15-softmax/MAGPIE/data/benchmarks/softmax-20261115/simfit/out-*.txt'
pos=collections.defaultdict(list)
for f in glob.glob(D):
    for l in open(f):
        if not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'k' not in d: continue
        if 'floor' in d: r=None
        elif 'sq' in d:
            se=[float(x) for x in d['se'].split(',')]; sw=[float(x) for x in d['sw'].split(',')]; sq=[float(x) for x in d['sq'].split(',')]
            if min(sw)<0: continue
            r=(int(d['obs']),se,sw,sq)
        else: continue
        pos[(d['game'],d['turn'],int(d['bag']),int(d['tiles']))].append((int(d['k']),r))
K=int(sys.argv[1]) if len(sys.argv)>1 else 150
P=[]
for key,rows in pos.items():
    rows.sort(key=lambda x:x[0])
    if len(rows)<K+1 or rows[0][0]!=0 or rows[0][1] is None: continue
    P.append((key,rows[0][1],[r for k,r in rows[1:K+1]]))
def logit(p):
    p=min(max(p,1e-4),1-1e-4); return math.log(p/(1-p))
def vals(r,kind,lam):
    o,se,sw,sq=r
    if kind=='static': return se
    if kind=='simeq': return [s+lam*(q-s) for s,q in zip(se,sq)]
    if kind=='simwin': return [logit(w) for w in sw]
def like(r,kind,lam,T,norm):
    if r is None: return 0.0
    v=vals(r,kind,lam); o=r[0]; m=max(v)
    if norm: return math.exp((v[o]-m)/T)/sum(math.exp((x-m)/T) for x in v)
    return math.exp((v[o]-m)/T)
def gain(p,kind,lam,T,norm):
    key,t,samples=p
    lt=like(t,kind,lam,T,norm); den=sum(like(r,kind,lam,T,norm) for r in samples)/len(samples)
    # Conservative (InfoNCE) form: the true leave counts in the denominator,
    # so a model cannot score by giving every sampled leave zero weight.
    tot=lt+den*len(samples)
    if lt<=0: return -20.0
    return math.log((len(samples)+1)*lt/tot)
bands=((61,100),(46,60),(31,45),(21,30),(14,20),(8,13),(1,7),(1,100))
TE=(0.125,0.25,0.5,1,1.5,2,3,4,5,6,8,10,12,16,24,32); TW=(0.0025,0.005,0.01,0.02,0.03,0.05,0.075,0.1,0.15,0.2,0.3,0.5,0.75,1,1.5,2.5)
models=[('static unnorm (current)','static',0,False,TE),('static softmax','static',0,True,TE)]
for lam in (0.25,0.5,0.75,1.0):
    models.append(('blend %.2f unnorm'%lam,'simeq',lam,False,TE)); models.append(('blend %.2f softmax'%lam,'simeq',lam,True,TE))
models+=[('sim win-logit unnorm','simwin',0,False,TW),('sim win-logit softmax','simwin',0,True,TW)]
print('positions',len(P),'samples per position',K)
print('%-26s'%'model'+''.join('%16s'%('bag %d-%d'%b) for b in bands))
print('%-26s'%'n'+''.join('%16d'%sum(b[0]<=p[0][2]<=b[1] for p in P) for b in bands))
for name,kind,lam,norm,grid in models:
    out=[]
    for b in bands:
        ps=[p for p in P if b[0]<=p[0][2]<=b[1]]
        if not ps: out.append('-'); continue
        best=max(((sum(gain(p,kind,lam,T,norm) for p in ps)/len(ps),T) for T in grid))
        out.append('%+.2f @%g'%best)
    print('%-26s'%name+''.join('%16s'%o for o in out))
