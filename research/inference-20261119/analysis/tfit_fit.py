import glob,re,math,collections,sys
G=[100000,0.5,1,1.5,2,3,4,5,6,8,10,12,16,24,32,-5]
rows=[]
for f in glob.glob(sys.argv[1]+'/*.out'):
    for l in open(f):
        if not l.endswith('\n'): continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if 'p' not in d: continue
        p=[float(x) for x in d['p'].split(',')]
        if len(p)!=len(G): continue
        rows.append((f,int(d['game']),int(d['bag']),int(d['tiles']),int(d['exch']),p))
print('positions',len(rows))
MIX=0.01
def ll(p,i): return math.log((1-MIX)*p[i]+MIX*p[0])   # mix with prior so hard5's zeros are finite
def table(name,sel,buckets,key):
    print('\n'+name); print('bucket | n | best T | gain at best | T2 | T4 | T8 | T16 | hard5   (nats over prior, per move)')
    for lo,hi in buckets:
        R=[r for r in rows if sel(r) and lo<=key(r)<=hi]
        if len(R)<30: continue
        g=[sum(ll(r[5],i)-math.log(r[5][0]) for r in R)/len(R) for i in range(len(G))]
        b=max(range(1,len(G)-1),key=lambda i:g[i])
        idx={t:G.index(t) for t in (2,4,8,16,-5)}
        print(f'{lo}-{hi} | {len(R)} | {G[b]} | {g[b]:+.3f} | '+' | '.join(f'{g[idx[t]]:+.3f}' for t in (2,4,8,16,-5)))
B=[(1,7),(8,13),(14,20),(21,30),(31,45),(46,60),(61,100)]
table('placements by bag',lambda r:not r[4],B,lambda r:r[2])
table('placements by tiles played',lambda r:not r[4],[(i,i) for i in range(1,7)],lambda r:r[3])
table('exchanges by bag',lambda r:r[4],[(1,20),(21,50),(51,100)],lambda r:r[2])
