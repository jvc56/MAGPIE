import sys, mmap, struct, collections
import numpy as np
fn = sys.argv[1]
f = open(fn,'rb'); m = mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
ver, rs, minp, _ = m[0], m[1], m[2], m[3]
nb, ne = struct.unpack_from('<II', m, 4)
off = 12 + 4*(nb+1)
esz = (len(m)-off)//ne
print(f"version={ver} rack={rs} buckets={nb:,} entries={ne:,} entry_size={esz} B rem={(len(m)-off)%ne}")
print(f"file={len(m)/1e9:.3f} GB  bucket array={4*(nb+1)/1e6:.1f} MB  entries={ne*esz/1e9:.3f} GB")
E = np.frombuffer(m, dtype=np.uint8, count=ne*esz, offset=off).reshape(ne, esz)
lp = E[:, :384].reshape(ne,128,3).astype(np.int32)
lv = lp[:,:,0] | (lp[:,:,1]<<8) | (lp[:,:,2]<<16)
lv = (lv ^ (1<<23)) - (1<<23)
print(f"leave value range: {lv.min()} .. {lv.max()} (equity units, 1/1000 pt)")
print(f"index0 unique values: {np.unique(lv[:,0])[:5]}  index127 unique count: {len(np.unique(lv[:,127]))}")
u = np.unique(lv)
print(f"distinct leave values in table: {len(u):,}")
g = np.gcd.reduce(np.abs(u[u!=0]))
print(f"gcd of values: {g}; fits int16 after /gcd: {np.abs(u//g).max() < 32768}")
br = E[:, esz-16:]
lo = br[:, :8].copy().view('<u8').ravel(); hi = br[:, 8:].copy().view('<u8').ravel()
counts = np.zeros((ne, 32), dtype=np.int64)
for ml in range(32):
    sh = ml*4
    counts[:,ml] = ((lo >> np.uint64(sh)) & np.uint64(15)) if sh < 64 else ((hi >> np.uint64(sh-64)) & np.uint64(15))
tot = counts.sum(1)
print(f"tiles per rack check: min {tot.min()} max {tot.max()}")
blanks = counts[:,0]
for b in range(3): print(f"  {b} blanks: {np.sum(blanks==b):,} entries")
distinct = np.prod(counts+1, axis=1)
print(f"distinct sub-multisets per rack: mean {distinct.mean():.1f} (of 128), all-distinct-letter racks {np.mean(distinct==128)*100:.1f}%")
print(f"leave bytes if storing only distinct sub-multisets @3B: {distinct.sum()*3/1e9:.3f} GB vs {ne*384/1e9:.3f} GB")
# distinct size<=6 leaves globally ~ KLV size
# Draw-probability mass coverage (full 100-tile bag proxy)
import csv, math
ld = sys.argv[2]
cnt = [int(r[2]) for r in csv.reader(open(ld)) if r]
cnt = cnt + [0]*(32-len(cnt))
from math import comb
total = comb(sum(cnt), 7)
lw = np.zeros(ne)
for ml in range(32):
    lut = np.array([math.log(comb(cnt[ml], k)) if k <= cnt[ml] else -np.inf for k in range(16)])
    lw += lut[counts[:, ml]]
p = np.exp(lw - math.log(total))
print(f"probability mass of all entries: {p.sum():.4f}")
ps = np.sort(p)[::-1]; cs = np.cumsum(ps)
for frac in (0.01, 0.05, 0.10, 0.20, 0.30, 0.50):
    k = int(ne*frac); print(f"  top {frac*100:4.0f}% of racks ({k:>9,} entries, {k*esz/1e6:7.0f} MB): {cs[k-1]*100:5.1f}% of draws")
for target in (0.90, 0.95, 0.99, 0.999):
    k = int(np.searchsorted(cs, target))+1; print(f"  {target*100:.1f}% of draws needs {k:,} entries ({k/ne*100:.1f}%, {k*esz/1e6:.0f} MB)")
