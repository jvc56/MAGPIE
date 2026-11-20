import re,math,collections,statistics as st,sys,glob
D=sys.argv[1]
tc={}
for f in glob.glob(D+'/replay/out-*.txt'):
    for l in open(f):
        if not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l)); tc[(d['game'],int(d['turn']))]=d
rec=collections.defaultdict(dict);done=set();car={}
for l in open(D+'/run.log',errors='replace'):
    if not l.endswith('\n'): continue
    t=l.split(' ',1)[0]
    if t in('INFDEC_ARM','INFDEC_GAME','INFDEC_CARRY'):
        d=dict(re.findall(r'(\w+)=(\S+)',l)); k=(d.get('game'),int(d.get('turn',0)))
        if t=='INFDEC_ARM':
            if d['same']=='1' or 'oracle_seconds' in d: rec[k][d['arm']]=d
        elif t=='INFDEC_GAME': done.add(d['game'])
        else: car[k]=d
def mv(d): return d['move'] if d['same']=='0' else None
def g(d,f,s): return s*float(d[f]) if d['same']=='0' else 0.0
rows=[];miss=0;mism=0
for k,r in rec.items():
    if k[0] not in done or 'm' not in r or 'g' not in r or int(r['m']['bag'])<8: continue
    c=car.get(k,{}); 
    if int(c.get('used',0))<1: continue
    t=tc.get((k[0],k[1]-1))
    if t is None: miss+=1; continue
    if int(t['hist'])<1: mism+=1; continue
    p0,p1=float(t['p0']),float(t['p1'])
    if p0<=0 or p1<=0: continue
    diff=mv(r['m'])!=mv(r['g'])
    rows.append(dict(lr=math.log(p1/p0),diff=diff,
       dw=(g(r['m'],'sim_dwin',100)-g(r['g'],'sim_dwin',100)) if diff else 0,
       du=(g(r['m'],'sim_dutility',100)-g(r['g'],'sim_dutility',100)) if diff else 0,
       de=(g(r['m'],'sim_dequity',1)-g(r['g'],'sim_dequity',1)) if diff else 0,
       lp1=math.log(p1),hist=int(t['hist'])))
print('decisions with carry-over in use',len(rows),'| no replay row',miss,'| replay had no history',mism)
print('mean ln(carry/schedule weight on true leave) %+.3f'%st.mean(r['lr'] for r in rows))
def line(name,R):
    d=[r for r in R if r['diff']]
    if len(d)<2: print(f'  {name:34s} n {len(R):5d} differ {len(d)}'); return
    se=lambda f:1.96*st.pstdev([r[f] for r in d])/math.sqrt(len(d))
    print(f'  {name:34s} n {len(R):5d} | differ {len(d):3d} ({100*len(d)/len(R):4.1f}%) | per differing: win {st.mean(r["dw"] for r in d):+.2f} ±{se("dw"):.2f} | utility {st.mean(r["du"] for r in d):+.2f} ±{se("du"):.2f} | equity {st.mean(r["de"] for r in d):+.2f} ±{se("de"):.2f} | per decision win {sum(r["dw"] for r in R)/len(R):+.4f}')
print('-- by how carry-over moved the weight on the true leave')
for name,f in (('cut by over 2x (ln < -0.69)',lambda r:r['lr']<-0.693),('cut 1.1x to 2x',lambda r:-0.693<=r['lr']<-0.095),('about unchanged (within 1.1x)',lambda r:abs(r['lr'])<=0.095),('raised 1.1x to 2x',lambda r:0.095<r['lr']<=0.693),('raised over 2x',lambda r:r['lr']>0.693),('ALL cut (ln<0)',lambda r:r['lr']<0),('ALL raised (ln>0)',lambda r:r['lr']>0)):
    line(name,[r for r in rows if f(r)])
d=[r for r in rows if r['diff']]
x=[r['lr'] for r in d]; y=[r['dw'] for r in d]
mx,my=st.mean(x),st.mean(y); cov=sum((a-mx)*(b-my) for a,b in zip(x,y)); rr=cov/math.sqrt(sum((a-mx)**2 for a in x)*sum((b-my)**2 for b in y))
print('differing decisions %d | correlation of ln ratio with win-pp delta %+.3f (se ~%.3f) | slope %+.2f win pp per nat'%(len(d),rr,1/math.sqrt(len(d)),cov/sum((a-mx)**2 for a in x)))
print('-- big outcomes among differing decisions: mean ln ratio')
for name,f in (('carry worse by 5pp+',lambda r:r['dw']<=-5),('worse 0-5',lambda r:-5<r['dw']<0),('better 0-5',lambda r:0<r['dw']<5),('better 5pp+',lambda r:r['dw']>=5)):
    R=[r for r in d if f(r)]; print(f'  {name:20s} n {len(R):3d} | mean ln ratio {st.mean(r["lr"] for r in R):+.3f} ±{1.96*st.pstdev([r["lr"] for r in R])/math.sqrt(len(R)):.3f} | truth raised in {100*sum(r["lr"]>0 for r in R)/len(R):.0f}%')
nd=[r for r in rows if not r['diff']]
print('  same move chosen       n %d | mean ln ratio %+.3f | truth raised in %.0f%%'%(len(nd),st.mean(r['lr'] for r in nd),100*sum(r['lr']>0 for r in nd)/len(nd)))
print('  mean |ln ratio|: differing %.3f, same %.3f'%(st.mean(abs(r['lr']) for r in d),st.mean(abs(r['lr']) for r in nd)))
