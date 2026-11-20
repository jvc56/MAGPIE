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
    G=len(r); se=math.sqrt(G/(G-1)*sum(v*v for v in r.values()))/len(x)
    return m,se
keys=[k for k in rec if k[0] in done and bag[k]>=8]
print('games',len(done),'bag>=8 decisions',len(keys))
for t,a,b in (('m - h','m','h'),('m - g','m','g'),('m vs none','m',None),('g vs none','g',None),('h vs none','h',None),('g - h','g','h')):
    ks=[k for k in keys if a in rec[k] and (b is None or b in rec[k])]
    g=[k[0] for k in ks]
    out=[]
    for i in range(3):
        m,se=cl([val(k,a,b,i) for k in ks],g); out.append('%+.4f ±%.4f (z %.2f)'%(m,1.96*se,m/se))
    print(t,' | '.join(out))
