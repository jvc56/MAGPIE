import re,sys,subprocess,collections,json
def physical(s): return ''.join('?' if t.islower() else t for t in s)
def positions(path):
    out=[]; cur=[]
    for line in open(path,errors='replace'):
        if not line.endswith('\n'): continue
        if line.startswith('INFDEC_GAME '): out+=cur; cur=[]; continue
        if not line.startswith('INFDEC_POS '): continue
        row=dict(re.findall(r'(\w+)=(\S+)',line))
        if row['turn']=='0':
            board=[['.']*15 for _ in range(15)]; scores=[0,0]; zeros=0; cur=[]
        seat=int(row['seat'])
        bt='/'.join(re.sub(r'\.+',lambda m:str(len(m[0])),''.join(r)) for r in board)
        cgp=f"{bt} {row['rack']}/{row['opp_rack']} {scores[seat]}/{scores[1-seat]} {zeros}"
        cur.append((cgp,row))
        move=row['0_move']
        if move=='pass' or move.startswith('(exch'):
            zeros+=1; continue
        coords,word,points=move.split('_')
        across=coords[0].isdigit()
        col=ord(coords[-1] if across else coords[0])-65
        rank=int(coords[:-1] if across else coords[1:])-1
        notation=re.sub(r'\(([^)]+)\)',lambda m:'.'*len(m[1]),word)
        for tile,logged in zip(word.replace('(','').replace(')',''),notation):
            assert board[rank][col]==(tile if logged=='.' else '.')
            board[rank][col]=tile; rank+=not across; col+=across
        scores[seat]+=int(points); zeros=0 if int(points) else zeros+1
    return out
allpos=[]
for p in sys.argv[2:]: allpos+=positions(p)
eng=subprocess.Popen([sys.argv[1],'infrank'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
res=collections.defaultdict(list)
for cgp,row in allpos:
    arms=[a for a in '0pslt' if a+'_move' in row]
    eng.stdin.write(cgp+'\t'+' '.join(row[a+'_move'] for a in arms)+'\n'); eng.stdin.flush()
    while True:
        l=eng.stdout.readline()
        if l.startswith('RANK'): break
        if not l: raise SystemExit('engine died')
    v=list(map(int,l.split()[1:]))
    for a,r in zip(arms,v[:-1]): res[a].append((r,v[-1],int(row['bag']),row['eligible']))
eng.stdin.close()
json.dump(res,open(sys.argv[1]+'.ranks.json','w'))
names={'0':'control (no inference)','p':'perfect information','s':'softmax T8','l':'kept leave','t':'top static'}
edges=[(1,1),(2,2),(3,5),(6,10),(11,20),(21,50),(51,100),(101,400)]
for a in '0psl':
    x=[r for r,_,_,e in res[a] if (a=='0' or e=='1')]
    if not x: continue
    n=len(x); nf=sum(r==0 for r in x); x=[r for r in x if r>0]; xs=sorted(x)
    print(f"{names[a]}: n={n} not found={nf} mean rank {sum(x)/len(x):.1f} median {xs[len(xs)//2]} p90 {xs[int(.9*len(xs))]} p99 {xs[int(.99*len(xs))]} max {xs[-1]}")
    print('   '+'  '.join(f"{lo}{'' if lo==hi else '-'+str(hi)}: {100*sum(lo<=r<=hi for r in x)/len(x):.1f}%" for lo,hi in edges))
