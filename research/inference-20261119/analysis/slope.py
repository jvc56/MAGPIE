import re,math,collections,statistics as st,sys
D='data/benchmarks/softmax-20261115/decide-fixed100k-t8/run.log'
bag={};sl=collections.defaultdict(list)
pend={}
for l in open(D,errors='replace'):
    if l.startswith('INFDEC_SIM') and ' arm=0 ' in l:
        m=re.search(r'game=(\d+) turn=(\d+)',l); k=(m.group(1),m.group(2))
        xs=[];ys=[];ws=[]
        for tok in l.split()[6:]:
            p=tok.split(',')
            if len(p)<9: continue
            try: n=float(p[1]);w=float(p[3]);s=float(p[7])
            except: continue
            if n<2000 or not 0.02<w<0.98: continue
            xs.append(s);ys.append(math.log(w/(1-w)));ws.append(n)
        if len(xs)>=3 and max(xs)-min(xs)>2:
            W=sum(ws);mx=sum(a*b for a,b in zip(ws,xs))/W;my=sum(a*b for a,b in zip(ws,ys))/W
            v=sum(a*(x-mx)**2 for a,x in zip(ws,xs))
            if v>0: pend[k]=sum(a*(x-mx)*(y-my) for a,x,y in zip(ws,xs,ys))/v
    elif l.startswith('INFDEC_POS'):
        m=re.search(r'game=(\d+) turn=(\d+).* bag=(\d+)',l); k=(m.group(1),m.group(2))
        if k in pend: bag[k]=int(m.group(3))
knots=[(2,2.5),(7,0.45),(14,0.14),(20,0.075),(30,0.047),(45,0.034),(60,0.029),(86,0.021)]
def tau(b):
    if b<=2: return 2.5
    for i in range(1,len(knots)):
        if b<=knots[i][0]:
            w=(b-knots[i-1][0])/(knots[i][0]-knots[i-1][0]); return math.exp((1-w)*math.log(knots[i-1][1])+w*math.log(knots[i][1]))
    return 0.021
B=[(1,7),(8,13),(14,20),(21,30),(31,45),(46,60),(61,100)]
print('bag | n | logit per equity pt (median) | Macondo tau (fitted) | capped 0.1 | T-equivalent in pts: fitted | capped')
for lo,hi in B:
    ks=[k for k in pend if k in bag and lo<=bag[k]<=hi]
    if not ks: continue
    s=st.median(pend[k] for k in ks); t=st.median(tau(bag[k]) for k in ks); c=min(t,0.1)
    print(f'{lo}-{hi} | {len(ks)} | {s:.4f} | {t:.3f} | {c:.3f} | {t/s:.1f} | {c/s:.1f}')
