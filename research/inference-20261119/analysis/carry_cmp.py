# Progress report for decide-fixed100k-prod: softmax T8 (s) and hard margin 5
# (h, main's default) against the no-inference control, plus s against h on
# the same decisions. Gains are per decision; +/- is a game-clustered 95%
# interval. Columns: win pp | utility pp | equity.
import re,math,collections,statistics as st,sys
D=sys.argv[1] if len(sys.argv)>1 else 'data/benchmarks/softmax-20261115/decide-fixed100k-prod/run.log'
rec=collections.defaultdict(dict);done=set();prev={};bag={}
for l in open(D,errors='replace'):
    if not l.endswith('\n'): continue
    if l.startswith('INFDEC_ARM'):
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'same' in d and (d['same']=='1' or 'oracle_seconds' in d):
            rec[(d['game'],int(d['turn']))][d['arm']]=d; bag[(d['game'],int(d['turn']))]=int(d['bag'])
    elif l.startswith('INFDEC_GAME'): done.add(dict(re.findall(r'(\w+)=(\S+)',l))['game'])
    elif l.startswith('INFDEC_POS'):
        d=dict(re.findall(r'(\w+)=(\S+)',l)); prev[(d['game'],int(d['turn'])+1)]=d['0_move']
F=(('sim_dwin',100),('sim_dutility',100),('sim_dequity',1))
def gain(d,i):
    return F[i][1]*float(d[F[i][0]]) if d['same']=='0' else 0.0
def val(k,a,b,i):
    r=rec[k]
    if b is None: return gain(r[a],i)
    if r[a]['same']=='0' and r[b]['same']=='0' and r[a]['move']==r[b]['move']: return 0.0
    return gain(r[a],i)-gain(r[b],i)
def cl(x,g):
    m=st.mean(x); r=collections.defaultdict(float)
    for v,gg in zip(x,g): r[gg]+=v-m
    G=len(r)
    if G<2: return '%+.3f ±n/a'%m
    return '%+.3f ±%.3f'%(m,1.96*math.sqrt(G/(G-1)*sum(v*v for v in r.values()))/len(x))
def show(name,keys,a,b):
    keys=[k for k in keys if a in rec[k] and (b is None or b in rec[k])]
    if not keys: print(name,'n 0'); return
    g=[k[0] for k in keys]
    if b is None: nd=sum(rec[k][a]['same']=='0' for k in keys)
    else: nd=sum((rec[k][a].get('move') if rec[k][a]['same']=='0' else None)!=(rec[k][b].get('move') if rec[k][b]['same']=='0' else None) for k in keys)
    print(name,'n %d'%len(keys),'differ %d (%.1f%%)'%(nd,100*nd/len(keys)),*[cl([val(k,a,b,i) for k in keys],g) for i in range(3)],sep=' | ')
keys=[k for k in rec if k[0] in done]
print('complete games',len(done),'decisions',len(keys))
ex=set(k for k in keys if prev.get(k,'').startswith('(exch'))
for title,a,b in (('carry - hard5','m','h'),('carry - schedule','m','g')):
    print('==',title)
    show('all',keys,a,b)
    show('after exchange',[k for k in keys if k in ex],a,b); show('after placement',[k for k in keys if k not in ex],a,b)
    for lo,hi in ((51,100),(21,50),(8,20),(1,7)):
        show(f'bag {lo}-{hi}',[k for k in keys if lo<=bag[k]<=hi],a,b)
