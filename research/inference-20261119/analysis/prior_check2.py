# For moves that play exactly one copy of a letter: how often did the player
# keep another copy, and what does each prior formula predict?
import re,math,collections,sys
from math import comb
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
    w=mv.split('_')[1]; w=re.sub(r'\([^)]*\)','',w); return cnt(w)
def zsum(N,P,size,skip=None,only0=None):
    poly=[1]+[0]*size
    for t in LET:
        if t==only0: term=[comb(N[t],P[t])]+[0]*size
        else: term=[comb(N[t],P[t]+c) for c in range(size+1)]
        nxt=[0]*(size+1)
        for a in range(size+1):
            if poly[a]:
                for c in range(size+1-a):
                    if term[c]: nxt[a+c]+=poly[a]*term[c]
        poly=nxt
    return poly[size]
agg=collections.defaultdict(lambda:[0,0.0,0.0,0])  # letter -> n, old, new, actual
for g in sorted(done):
    board=collections.Counter()
    for d in sorted(games[g],key=lambda d:int(d['turn'])):
        mv=d['0_move']; R=cnt(d['rack']); O=cnt(d['opp_rack'])
        if mv.startswith('(exch') or 'pass' in mv.lower(): continue
        P=placed(mv); N={t:DIST[t]-board[t]-O[t] for t in LET}; L=R-P; size=sum(L.values())
        if size>0:
            U=sum(N.values())-sum(P.values()); Z=None
            for t in LET:
                if P[t]==1:
                    rem=N[t]-1
                    p_old=1-comb(U-rem,size)/comb(U,size)
                    if Z is None: Z=zsum(N,P,size)
                    p_new=1-zsum(N,P,size,only0=t)/Z
                    for key in (t,'all'):
                        a=agg[key]; a[0]+=1; a[1]+=p_old; a[2]+=p_new; a[3]+= L[t]>0
        board+=P
print('letter | moves playing exactly one | kept another: actual | current formula | full-rack formula')
for key in ['all','S','?','E','A','I','O','R','T','N','D','L','U']:
    n,o,nw,a=agg[key]
    if n: print('%-4s | %6d | %.3f | %.3f | %.3f'%(key,n,a/n,o/n,nw/n))
