# Paired comparison of the leave-prediction benchmark with the current prior
# (old) and the full-rack prior (new): same games, same observed moves.
import re,sys,math,collections
def load(p):
    d={}
    for l in open(p,errors='replace'):
        if l.startswith('INFBENCH decision'):
            kv=dict(re.findall(r'(\w+)=(\S+)',l))
            d[(kv['policy'],kv['pair'],kv['game'],kv['turn'],kv['mode'])]=kv
    return d
old=load(sys.argv[1]); new=load(sys.argv[2])
keys=[k for k in old if k in new]
print('paired mode-observations',len(keys))
modes=['hard5','softnocutofft8','softnocutofft4','softnocutofft16']
for pol in ('static','pat','sim2'):
    for m in modes:
        ks=[k for k in keys if k[0]==pol and k[4]==m]
        if not ks: continue
        out=[]
        for f in ('mixed_nll','klv_mse','tile_mse','duplicate_mse'):
            by=collections.defaultdict(float); tot=0
            vals=[float(new[k][f])-float(old[k][f]) for k in ks]
            mean=sum(vals)/len(vals)
            for k,v in zip(ks,vals): by[(k[1],)]+=v-mean
            G=len(by); se=math.sqrt(G/(G-1)*sum(v*v for v in by.values()))/len(vals) if G>1 else float('nan')
            out.append('%s %+.4f ±%.4f'%(f,mean,1.96*se))
        base=sum(float(old[k]['mixed_nll']) for k in ks)/len(ks)
        print('%-6s %-16s n %5d old_nll %.3f | new-old: %s'%(pol,m,len(ks),base,' | '.join(out)))
