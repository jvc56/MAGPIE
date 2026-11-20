# Offline check of the inference prior on logged sim-player moves.
# old: ways to draw the kept tiles from the unseen pool without the played tiles.
# new: ways to draw the full rack (played + kept) from the unseen pool with them.
import re,math,collections,sys
from math import comb,log
DIST=dict(A=9,B=2,C=2,D=4,E=12,F=2,G=3,H=2,I=9,J=1,K=1,L=4,M=2,N=6,O=8,P=2,Q=1,R=6,S=4,T=6,U=4,V=2,W=2,X=1,Y=2,Z=1)
DIST['?']=2
LET=sorted(DIST)
f=sys.argv[1]
games=collections.defaultdict(list); done=set()
for l in open(f,errors='replace'):
    if l.startswith('INFDEC_POS'):
        d=dict(re.findall(r'(\w+)=(\S+)',l)); games[int(d['game'])].append(d)
    elif l.startswith('INFDEC_GAME'):
        done.add(int(dict(re.findall(r'(\w+)=(\S+)',l))['game']))
def cnt(s):
    c=collections.Counter()
    for ch in s: c['?' if ch=='?' or ch.islower() else ch]+=1
    return c
def placed(mv):
    # "<pos>_<word>_<score>", through tiles in parentheses, blanks lowercase
    w=mv.split('_')[1]; w=re.sub(r'\([^)]*\)','',w); return cnt(w)
def norm_new(N,P,size):
    poly=[1]+[0]*size
    for t in LET:
        term=[comb(N[t],P[t]+c) for c in range(size+1)]
        nxt=[0]*(size+1)
        for a in range(size+1):
            if poly[a]:
                for c in range(size+1-a):
                    if term[c]: nxt[a+c]+=poly[a]*term[c]
        poly=nxt
    return poly[size]
rows=[]; bad=0
for g in sorted(done):
    board=collections.Counter()
    for d in sorted(games[g],key=lambda d:int(d['turn'])):
        mv=d['0_move']; R=cnt(d['rack']); O=cnt(d['opp_rack'])
        if mv.startswith('(exch') or 'pass' in mv.lower(): continue
        P=placed(mv)
        N={t:DIST[t]-board[t]-O[t] for t in LET}
        L=R-P
        if any(v<0 for v in N.values()) or sum(P.values())+sum(L.values())!=sum(R.values()) or any(N[t]<R[t] for t in LET):
            bad+=1; board+=P; continue
        size=sum(L.values())
        if size>0:
            old=1
            for t in LET: old*=comb(N[t]-P[t],L[t])
            old/=comb(sum(N.values())-sum(P.values()),size)
            new=1
            for t in LET: new*=comb(N[t],P[t]+L[t])
            new/=norm_new(N,P,size)
            shared=sum(L[t] for t in LET if P[t]>0)
            # predicted number of kept tiles whose letter was also played
            U=sum(N.values())-sum(P.values())
            exp_old=size*sum(N[t]-P[t] for t in LET if P[t]>0)/U
            rows.append((g,log(new/old),shared,exp_old,size,int(d['bag']),sum(P.values())))
        board+=P
print('placements scored',len(rows),'skipped inconsistent',bad,'games',len(done))
def cl(vals,gs):
    n=len(vals); m=sum(vals)/n; r=collections.defaultdict(float)
    for v,g in zip(vals,gs): r[g]+=v-m
    G=len(r); return m,1.96*math.sqrt(G/(G-1)*sum(v*v for v in r.values()))/n
def show(name,rs):
    m,ci=cl([r[1] for r in rs],[r[0] for r in rs])
    print('%-34s n %6d | gain of new over old %+.4f ±%.4f nats per move'%(name,len(rs),m,ci))
show('all placements',rows)
show('kept a letter also played',[r for r in rows if r[2]>0])
show('kept no letter also played',[r for r in rows if r[2]==0])
for lo,hi in ((1,2),(3,4),(5,6)):
    show('kept %d-%d tiles'%(lo,hi),[r for r in rows if lo<=r[4]<=hi])
act=sum(r[2] for r in rows)/len(rows); eo=sum(r[3] for r in rows)/len(rows)
print('kept tiles matching a played letter, per move: actual %.4f, old prior expects %.4f'%(act,eo))
print('share of moves keeping at least one such tile: actual %.4f'%(sum(r[2]>0 for r in rows)/len(rows)))
