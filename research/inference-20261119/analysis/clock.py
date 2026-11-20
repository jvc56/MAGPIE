import re,sys,statistics as st
d='data/benchmarks/softmax-20261115/'
def q(v,p): v=sorted(v); return v[min(len(v)-1,int(p*len(v)))]
for r in ['decide-15s','decide-15s-ext','decide-15s-feat','decide-15s-soft','decide-15s-sampler','decide-fixed100k-t8']:
    it={'0':[], 's':[]}; se={'0':[], 's':[]}; rate=[]
    for l in open(d+r+'/run.log'):
        if not l.startswith('INFDEC_POS') or ' eligible=1' not in l: continue
        kv=dict(x.split('=',1) for x in l.split()[1:])
        for a in '0s':
            if a+'_iterations' in kv:
                i=int(kv[a+'_iterations']); s=float(kv[a+'_seconds']); it[a].append(i); se[a].append(s)
                if a=='0' and s>0.2: rate.append(i/s)
    for a in '0s':
        v=it[a]; s=se[a]
        if not v: continue
        print(r,a,'n',len(v),'iters mean %.0f med %.0f p90 %.0f max %.0f'%(st.mean(v),q(v,.5),q(v,.9),max(v)),
              '| secs mean %.2f med %.2f p90 %.2f'%(st.mean(s),q(s,.5),q(s,.9)),'| frac>=14.5s %.3f'%(sum(x>=14.5 for x in s)/len(s)))
    print('   control iters/sec median %.0f mean %.0f'%(q(rate,.5),st.mean(rate)))
