import glob,re,math,sys
G=[100000,0.5,1,1.5,2,3,4,5,6,8,10,12,16,24,32,-5]
def sched(bag):
    for lo,t in ((61,1.5),(46,2),(31,3),(21,4),(14,5),(8,6),(1,12)):
        if bag>=lo: return t
def summ(name,lifts):
    n=len(lifts); f=lambda c:100*sum(1 for x in lifts if x<c)/n
    z=100*sum(1 for x in lifts if x==0)/n
    up=100*sum(1 for x in lifts if x>1)/n
    print(f'{name:22s} n {n:5d} | raised {up:5.1f}% | cut >2x {f(0.5):5.1f}% | >10x {f(0.1):5.1f}% | >100x {f(0.01):5.1f}% | zero {z:5.1f}%')
for tag in ('prod','t8'):
    rows=[]
    for f in glob.glob(S_:=sys.argv[1]+'/tfit/'+tag+'-*.out'):
        for l in open(f):
            if not l.startswith('TFIT ') or not l.endswith('\n'): continue
            d=dict(re.findall(r'(\w+)=(\S+)',l))
            if 'p' not in d: continue
            p=[float(x) for x in d['p'].split(',')]
            if len(p)!=len(G) or p[0]<=0 or int(d['exch']): continue
            rows.append((int(d['bag']),p))
    print('== static kernels, placements,',tag,'games: weight on the true leave relative to prior')
    for lohi in ((1,100),(8,100),(1,7)):
        R=[r for r in rows if lohi[0]<=r[0]<=lohi[1]]
        print(' bag',lohi)
        summ('  hard5',[p[15]/p[0] for b,p in R])
        summ('  T8',[p[9]/p[0] for b,p in R])
        summ('  schedule',[p[G.index(sched(b))]/p[0] for b,p in R])
        summ('  T2',[p[4]/p[0] for b,p in R])
# carry
rows=[]
for f in glob.glob(sys.argv[2]+'/out-*.txt'):
    for l in open(f):
        if not l.startswith('TCARRY') or not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if int(d['hist'])<1: continue
        rows.append((int(d['bag']),int(d['hist']),int(d['kept']),float(d['p0']),float(d['p1'])))
print('== carry-over relative to schedule (moves with history)')
r=[b[4]/b[3] for b in rows if b[3]>0]
summ('  all',r)
for lo,hi in ((1,2),(3,4),(5,6)):
    summ(f'  kept {lo}-{hi}',[b[4]/b[3] for b in rows if b[3]>0 and lo<=b[2]<=hi])
for lo,hi in ((1,1),(2,2),(3,9)):
    summ(f'  history {lo}-{hi}',[b[4]/b[3] for b in rows if b[3]>0 and lo<=b[1]<=hi])
ln=sorted(math.log(x) for x in r if x>0)
q=lambda a:ln[int(a*(len(ln)-1))]
print('  ln ratio quantiles 1% %.2f 5%% %.2f 25%% %.2f 50%% %.2f 75%% %.2f 95%% %.2f 99%% %.2f mean %.3f'%(q(.01),q(.05),q(.25),q(.5),q(.75),q(.95),q(.99),sum(ln)/len(ln)))
