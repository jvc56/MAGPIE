import re,math,collections,statistics as st
D='data/benchmarks/softmax-20261115/decide-fixed100k-t8/run.log'
rows=[];done=set();prev={}
for l in open(D,errors='replace'):
    if not l.endswith('\n'): continue
    if l.startswith('INFDEC_ARM'):
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'same' in d and (d['same']=='1' or 'oracle_seconds' in d): rows.append(d)
    elif l.startswith('INFDEC_GAME'): done.add(dict(re.findall(r'(\w+)=(\S+)',l))['game'])
    elif l.startswith('INFDEC_POS'):
        d=dict(re.findall(r'(\w+)=(\S+)',l)); prev[(d['game'],int(d['turn'])+1)]=d['0_move']
rows=[r for r in rows if r['game'] in done]
def cl(x,g):
    m=st.mean(x); r=collections.defaultdict(float)
    for v,gg in zip(x,g): r[gg]+=v-m
    G=len(r); return '%+.3f ±%.3f'%(m,1.96*math.sqrt(G/(G-1)*sum(v*v for v in r.values()))/len(x))
def show(name,rs):
    g=[r['game'] for r in rs]
    print(name,'n',len(rs),'differ %d (%.1f%%)'%(sum(r['same']=='0' for r in rs),100*sum(r['same']=='0' for r in rs)/len(rs)),*[cl([sc*(float(r[f]) if r['same']=='0' else 0) for r in rs],g) for f,sc in (('sim_dwin',100),('sim_dutility',100),('sim_dequity',1))],sep=' | ')
print('games',len(done))
show('all',rows)
ex=[r for r in rows if prev.get((r['game'],int(r['turn'])),'').startswith('(exch')]
exs=set(id(r) for r in ex)
show('after exchange',ex); show('after placement',[r for r in rows if id(r) not in exs])
for lo,hi in ((51,100),(21,50),(8,20),(1,7)):
    show(f'bag {lo}-{hi}',[r for r in rows if lo<=int(r['bag'])<=hi])
