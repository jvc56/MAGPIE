import re,math,collections,statistics as st,sys
D=sys.argv[1]; A,B=sys.argv[2],sys.argv[3]
rec=collections.defaultdict(dict);done=set();pos={};car={}
for l in open(D,errors='replace'):
    if not l.endswith('\n'): continue
    t=l.split(' ',1)[0]
    if t in('INFDEC_ARM','INFDEC_GAME','INFDEC_POS','INFDEC_CARRY'):
        d=dict(re.findall(r'(\w+)=(\S+)',l)); k=(d.get('game'),int(d.get('turn',0)))
        if t=='INFDEC_ARM':
            if d['same']=='1' or 'oracle_seconds' in d: rec[k][d['arm']]=d
        elif t=='INFDEC_GAME': done.add(d['game'])
        elif t=='INFDEC_POS': pos[k]=d
        else: car[k]=d
def mv(d): return d['move'] if d['same']=='0' else None
def g(d,f='sim_dwin',s=100): return s*float(d[f]) if d['same']=='0' else 0.0
rows=[]
for k,r in rec.items():
    if k[0] not in done or A not in r or B not in r: continue
    bag=int(r[A]['bag'])
    if bag<8: continue
    diff=mv(r[A])!=mv(r[B])
    dw=g(r[A])-g(r[B]) if diff else 0.0
    du=(g(r[A],'sim_dutility')-g(r[B],'sim_dutility')) if diff else 0.0
    de=(g(r[A],'sim_dequity',1)-g(r[B],'sim_dequity',1)) if diff else 0.0
    pm=pos.get((k[0],k[1]-1),{}).get('0_move','')
    c=car.get(k,{})
    m=re.match(r'\(exch_(\w+)\)',pm); 
    if m: tiles=len(m.group(1)); score=0; ex=1
    else:
        mm=re.match(r'\S+?_([A-Za-z.]+)_(\d+)$',pm); ex=0
        score=int(mm.group(2)) if mm else -1
        tiles=7-int(c['kept_out']) if 'kept_out' in c else -1
    rows.append(dict(k=k,bag=bag,diff=diff,dw=dw,du=du,de=de,used=int(c.get('used',0)),kin=int(c.get('kept_in',0)),kout=int(c.get('kept_out',-1)),score=score,ex=ex,
        a_ctl=mv(r[A]) is None,b_ctl=mv(r[B]) is None,am=r[A].get('move','=ctl'),bm=r[B].get('move','=ctl'),pm=pm,p=pos.get(k,{})))
n=len(rows); df=[r for r in rows if r['diff']]
print(f'{A} - {B}: decisions {n}, differ {len(df)} ({100*len(df)/n:.1f}%), mean per decision {sum(r["dw"] for r in rows)/n:+.4f} win pp')
x=sorted(r['dw'] for r in df)
q=lambda a:x[int(a*(len(x)-1))]
print('among differing: mean %+.3f median %+.3f sd %.2f | better %d worse %d | sum of gains %+.1f sum of losses %+.1f'%(st.mean(x),st.median(x),st.pstdev(x),sum(v>0 for v in x),sum(v<0 for v in x),sum(v for v in x if v>0),sum(v for v in x if v<0)))
print('quantiles 1%% %+.2f 5%% %+.2f 25%% %+.2f 75%% %+.2f 95%% %+.2f 99%% %+.2f'%tuple(q(a) for a in(.01,.05,.25,.75,.95,.99)))
for lo,hi in ((-99,-5),(-5,-2),(-2,-1),(-1,0),(0,1),(1,2),(2,5),(5,99)):
    s=[v for v in x if lo<=v<hi]; print(f'  delta {lo:+d}..{hi:+d} pp: n {len(s):4d} sum {sum(s):+8.1f}')
def tab(title,key,groups):
    print('--',title,'(n | differ | differ% | win pp per decision | per differing decision | utility per differing | equity per differing)')
    for name,f in groups:
        R=[r for r in rows if f(r)]; d=[r for r in R if r['diff']]
        if not R: continue
        print(f'  {name:16s} {len(R):6d} | {len(d):4d} | {100*len(d)/len(R):4.1f}% | {sum(r["dw"] for r in R)/len(R):+.4f} | '+(f'{st.mean(r["dw"] for r in d):+.3f} ±{1.96*st.pstdev([r["dw"] for r in d])/math.sqrt(len(d)):.3f} | {st.mean(r["du"] for r in d):+.3f} | {st.mean(r["de"] for r in d):+.2f}' if len(d)>1 else 'n/a'))
tab('history depth used',None,[('none',lambda r:r['used']==0),('1',lambda r:r['used']==1),('2',lambda r:r['used']==2),('3+',lambda r:r['used']>=3)])
tab('tiles kept last turn (carried in)',None,[(str(i),lambda r,i=i:r['used']>0 and r['kin']==i) for i in range(1,7)])
tab('tiles kept this turn by opp',None,[(str(i),lambda r,i=i:r['kout']==i) for i in range(0,7)])
tab('opp last score',None,[('exchange',lambda r:r['ex']),('1-9',lambda r:not r['ex'] and 0<=r['score']<10),('10-19',lambda r:not r['ex'] and 10<=r['score']<20),('20-29',lambda r:not r['ex'] and 20<=r['score']<30),('30-49',lambda r:not r['ex'] and 30<=r['score']<50),('50+',lambda r:not r['ex'] and r['score']>=50)])
tab('which arm left the control move',None,[(A+' = control',lambda r:r['diff'] and r['a_ctl']),(B+' = control',lambda r:r['diff'] and r['b_ctl']),('neither',lambda r:r['diff'] and not r['a_ctl'] and not r['b_ctl'])])
print('-- 12 worst and 6 best differing decisions')
for r in sorted(df,key=lambda r:r['dw'])[:12]+sorted(df,key=lambda r:-r['dw'])[:6]:
    p=r['p']; print(f"  {r['dw']:+6.2f}pp eq {r['de']:+6.1f} g{r['k'][0]} t{r['k'][1]} bag {r['bag']} hist {r['used']} kin {r['kin']} opp_last {r['pm']} | rack {p.get('rack')} opp {p.get('opp_rack')} | {A}: {r['am']}  {B}: {r['bm']}")
