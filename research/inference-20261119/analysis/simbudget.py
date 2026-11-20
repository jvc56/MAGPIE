# Does disagreement on a small pilot predict where more sims pay? usage: simbudget.py <subdir or ''> <pilot racks>
import sys,math,statistics as st
import numpy as np
M=int(sys.argv[2]); sub=sys.argv[1]
src=open(__file__.replace('simbudget','simimpute3')).read().split("SETS=[")[0].replace("Ms=[int(x) for x in sys.argv[2:]]","Ms=[]")
exec(src)
rows=[]
for p in P:
    fit=[r for r in p[2][:M] if r is not None and r['sl']<=40]
    if len(fit)<8: continue
    x=np.array([r['sl'] for r in fit]); y=np.array([min(r['ml'],40.0) for r in fit])
    rows.append((p,float(np.sqrt(np.mean((y-x)**2)))))
ok=[p for p,_ in rows]
def run(fs):
    T=max(TE,key=lambda T:sum(score(p,T,f,M) for p,f in zip(ok,fs))); return [score(p,T,f,M) for p,f in zip(ok,fs)]
base=run([(lambda r:r['sl']) for p in ok]); sim=run([(lambda r:r['ml']) for p in ok]); imp=run([predictor(p,('let',),M) for p in ok])
gs=[a-b for a,b in zip(sim,base)]; gi=[a-b for a,b in zip(imp,base)]
order=sorted(range(len(rows)),key=lambda i:-rows[i][1])
print(sub or 'main (500 rollouts)','positions',len(rows),'| pilot of',M,'racks | full-sim gain %+.3f | pilot-imputed gain %+.3f'%(st.mean(gs),st.mean(gi)))
n=len(order); q=n//4
for j,name in enumerate(('most disagreement','second quarter','third quarter','least disagreement')):
    idx=order[j*q:(j+1)*q if j<3 else n]
    print('  %-18s rmse %4.1f-%4.1f | full-sim gain %+.2f ±%.2f | share of total gain %3.0f%% | pilot-imputed gain %+.2f'%(name,rows[idx[-1]][1],rows[idx[0]][1],st.mean(gs[i] for i in idx),1.96*st.stdev(gs[i] for i in idx)/math.sqrt(len(idx)),100*sum(gs[i] for i in idx)/sum(gs),st.mean(gi[i] for i in idx)))
for frac in (0.25,0.5,0.75):
    top=set(order[:int(n*frac)])
    mix=[gs[i] if i in top else gi[i] for i in range(n)]
    print('  full sims on the top %2.0f%% by disagreement, pilot imputation elsewhere: gain %+.3f'%(100*frac,st.mean(mix)))
