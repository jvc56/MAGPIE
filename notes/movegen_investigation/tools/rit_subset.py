"""Write a RIT holding only the most probable full racks (by full-bag draw
probability). Usage: rit_subset.py <in.rit> <letterdist.csv> <frac> <out.rit>
Rebuilds buckets with MAGPIE's bit_rack_mix_to_64 hash; verifies the hash
against the input file's own bucket layout first."""
import sys, struct, math, csv
import numpy as np
from math import comb
src, ldf, frac, dst = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
raw = np.fromfile(src, dtype=np.uint8)
hdr = raw[:4].tobytes()
nb, ne = struct.unpack_from('<II', raw[:12].tobytes(), 4)
starts = raw[12:12+4*(nb+1)].view('<u4')
off = 12 + 4*(nb+1)
esz = (raw.size - off) // ne
assert esz * ne + off == raw.size
E = raw[off:].reshape(ne, esz)
br = np.ascontiguousarray(E[:, esz-16:])
lo = br[:, :8].copy().view('<u8').ravel(); hi = br[:, 8:].copy().view('<u8').ravel()
M = (1 << 64) - 1
def mix(lo, hi):
    with np.errstate(over='ignore'):
        x = lo ^ hi
        x ^= (hi << np.uint64(17)) | (hi >> np.uint64(47))
        x ^= x >> np.uint64(33); x *= np.uint64(0xff51afd7ed558ccd)
        x ^= x >> np.uint64(33); x *= np.uint64(0xc4ceb9fe1a85ec53)
        x ^= x >> np.uint64(33)
    return x
h = mix(lo, hi)
# verify against the input's layout
bucket_of_entry = np.repeat(np.arange(nb, dtype=np.uint64), np.diff(starts).astype(np.int64))
assert np.array_equal(h & np.uint64(nb-1), bucket_of_entry), "hash mismatch"
# draw probability
cnt = [int(r[2]) for r in csv.reader(open(ldf)) if r]; cnt += [0]*(32-len(cnt))
lw = np.zeros(ne)
for ml in range(32):
    sh = ml*4
    c = ((lo >> np.uint64(sh)) if sh < 64 else (hi >> np.uint64(sh-64))) & np.uint64(15)
    lut = np.array([math.log(comb(cnt[ml], k)) if k <= cnt[ml] else -np.inf for k in range(16)])
    lw += lut[c.astype(np.int64)]
p = np.exp(lw - math.log(comb(sum(cnt), 7)))
k = max(1, int(round(ne * frac)))
keep = np.zeros(ne, bool); keep[np.argsort(-p, kind='stable')[:k]] = True
nb2 = max(16, 1 << (k-1).bit_length())
kidx = np.nonzero(keep)[0]
b2 = (h[kidx] & np.uint64(nb2-1)).astype(np.int64)
order = kidx[np.argsort(b2, kind='stable')]
counts = np.bincount(b2, minlength=nb2)
st2 = np.zeros(nb2+1, dtype='<u4'); st2[1:] = np.cumsum(counts)
with open(dst + '.tmp', 'wb') as f:
    f.write(hdr); f.write(struct.pack('<II', nb2, k)); f.write(st2.tobytes())
    f.write(E[order].tobytes())
import os; os.replace(dst + '.tmp', dst)
print(f"kept {k:,}/{ne:,} entries ({frac*100:.0f}%), mass {p[keep].sum()*100:.1f}%, buckets {nb2:,}, {os.path.getsize(dst)/1e6:.0f} MB -> {dst}")
