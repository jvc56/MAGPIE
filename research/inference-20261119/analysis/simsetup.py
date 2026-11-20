import math,re,statistics as st,sys
sys.argv=[sys.argv[0]]
exec(open(__file__.replace('simsetup','simfit')).read().split("bands=")[0])
G={}
for l in open(__file__.replace('simsetup.py','sgap.txt')):
    d=dict(re.findall(r'(\w+)=(\S+)',l)); G[(d['game'],d['turn'])]=float(d['gap'])
# correction = how much better the sim rates the observed play (vs its best alternative) than static does
def corr(r): return (r[3][r[0]]-max(r[3]))-(r[1][r[0]]-r[1][0])
def rep(name,ps):
    tc=[corr(p[1]) for p in ps]
    mean_s=[]; sd_s=[]; top=[]
    for p in ps:
        c=[corr(r) for r in p[2] if r is not None and r[1][0]-r[1][r[0]]<=30]
        if len(c)<20: continue
        mean_s.append(st.mean(c)); sd_s.append(st.stdev(c)); 
    print('%-10s n %3d | true rack: sim credits observed play %+.1f pts vs static (median %+.1f) | sampled racks: mean %+.1f, spread across racks (sd) %.1f | true minus sampled %+.1f'%(name,len(ps),st.mean(tc),st.median(tc),st.mean(mean_s),st.mean(sd_s),st.mean(tc)-st.mean(mean_s)))
rep('gap = 0',[p for p in P if G[(p[0][0],p[0][1])]==0]); rep('gap > 0',[p for p in P if G[(p[0][0],p[0][1])]>0]); rep('gap > 2',[p for p in P if G[(p[0][0],p[0][1])]>2])
