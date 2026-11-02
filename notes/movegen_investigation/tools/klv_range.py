import sys, struct, glob, os, math
import numpy as np
worst = 0
for fn in sorted(set(os.path.realpath(f) for f in sys.argv[1:])):
    b = open(fn,'rb').read()
    ks, = struct.unpack_from('<I', b, 0)
    o = 4 + 4*ks
    n, = struct.unpack_from('<I', b, o)
    v = np.frombuffer(b, '<f4', n, o+4).astype(np.float64)
    eq = np.round(v*1000).astype(np.int64)
    lo, hi = min(eq.min(), 0), max(eq.max(), 0)
    bits = math.ceil(math.log2(hi - lo + 1))
    worst = max(worst, bits)
    print(f"{os.path.basename(fn):28s} n={n:>8,} min={lo/1000:8.3f} max={hi/1000:8.3f} span={(hi-lo)/1000:8.3f} -> {bits} bits")
print("max bits needed:", worst)
