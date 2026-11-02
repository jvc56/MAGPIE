import sys, struct, math, csv
import numpy as np
from math import comb
raw = np.fromfile(sys.argv[1], dtype=np.uint8)
nb, ne = struct.unpack_from('<II', raw[:12].tobytes(), 4)
off = 12 + 4*(nb+1); esz = (raw.size-off)//ne
E = raw[off:].reshape(ne, esz)
br = np.ascontiguousarray(E[:, esz-16:])
lo = br[:, :8].copy().view('<u8').ravel(); hi = br[:, 8:].copy().view('<u8').ravel()
cnt = [int(r[2]) for r in csv.reader(open(sys.argv[2])) if r]; cnt += [0]*(32-len(cnt))
C = np.zeros((ne,32), np.int64); lw = np.zeros(ne)
for ml in range(32):
    sh = ml*4
    C[:,ml] = (((lo >> np.uint64(sh)) if sh < 64 else (hi >> np.uint64(sh-64))) & np.uint64(15)).astype(np.int64)
    lut = np.array([math.log(comb(cnt[ml], k)) if k <= cnt[ml] else -np.inf for k in range(16)])
    lw += lut[C[:,ml]]
p = np.exp(lw - math.log(comb(sum(cnt),7)))
d = np.prod(C+1, axis=1)
order = np.argsort(-p, kind='stable')
# ordered multiplicity pattern (composition of 7) count
comp = set()
for row in C[order[:200000]]:
    comp.add(tuple(x for x in row if x))
print(f"distinct multiplicity patterns seen (top 200k): {len(comp)}")
print(f"{'kept':>5} {'entries':>10} {'distinct/rack':>13} {'leaves 24b':>10} {'dedupe 24b':>10} {'17b':>8} {'dedupe+17b':>10}   (MB, leave bytes only)")
for frac in (0.2, 0.5, 1.0):
    k = int(round(ne*frac)); sel = order[:k]; ds = d[sel]
    full24 = k*128*3; dd24 = ds.sum()*3; b17 = k*128*17/8; dd17 = ds.sum()*17/8
    tot = k*esz
    print(f"{frac*100:4.0f}% {k:>10,} {ds.mean():13.1f} {full24/1e6:10.0f} {dd24/1e6:10.0f} {b17/1e6:8.0f} {dd17/1e6:10.0f}   file {tot/1e6:.0f} MB; dedupe saves {(full24-dd24)/tot*100:.1f}% of file, 17b saves {(full24-b17)/tot*100:.1f}%, both {(full24-dd17)/tot*100:.1f}%")
