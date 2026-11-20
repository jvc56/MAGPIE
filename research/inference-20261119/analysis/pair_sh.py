import math,collections,sys
f='data/benchmarks/softmax-20261115/decide-15s-ext/run.log'
done=set(); rec=collections.defaultdict(dict)
for l in open(f):
    if l.startswith('INFDEC_GAME'): done.add(int(l.split()[2].split('=')[1]))
    elif l.startswith('INFDEC_ARM'):
        kv=dict(x.split('=',1) for x in l.split()[1:])
        v=(0.0,0.0,0.0) if kv['same']=='1' else (float(kv['sim_dwin']),float(kv['sim_dutility']),float(kv['sim_dequity']))
        rec[(int(kv['game']),int(kv['turn']))][kv['arm']]=v
def rep(a,b):
    keys=[k for k in rec if k[0] in done and a in rec[k] and (b is None or b in rec[k])]
    G=len({k[0] for k in keys}); n=len(keys); out=[]
    for i,sc in ((0,100),(1,100),(2,1)):
        by=collections.defaultdict(float)
        for k in keys: by[k[0]]+=rec[k][a][i]-(rec[k][b][i] if b else 0)
        m=sum(by.values())/n; se=math.sqrt(G/(G-1)*sum(v*v for v in by.values()))/n
        out.append('%+.3f ±%.3f'%(m*sc,1.96*se*sc))
    print('%s vs %s | n %d games %d | win %s | utility %s | equity %s'%(a,b or 'control',n,G,*out))
for a,b in (('s',None),('h',None),('s','h'),('a',None),('b',None),('s','a'),('b','s')): rep(a,b)
