"""Compare a v12 RIT (24-bit leaves inline) with a v13 RIT (variable-width
leave block ahead of each entry): buckets, every fixed field, every leave."""
import sys, struct
import numpy as np
a = np.fromfile(sys.argv[1], dtype=np.uint8); b = np.fromfile(sys.argv[2], dtype=np.uint8)
assert a[0] == 12 and b[0] == 13, (a[0], b[0])
assert a[1] == b[1] and a[2] == b[2], "rack size / min played differ"
nba, nea = struct.unpack_from('<II', a[:12].tobytes(), 4)
bits = int(b[3]); nbb, neb, base = struct.unpack_from('<IIi', b[:16].tobytes(), 4)
print(f"v12: buckets={nba:,} entries={nea:,}   v13: bits={bits} base={base} buckets={nbb:,} entries={neb:,}")
assert (nba, nea) == (nbb, neb)
sa = a[12:12+4*(nba+1)]; sb = b[16:16+4*(nbb+1)]
assert np.array_equal(sa, sb), "bucket_starts differ"
offa = 12+4*(nba+1); offb = 16+4*(nbb+1)
ESZ_A = 584; LB = ((128*bits + 63)//64)*8; ESZ_B = LB + 200
assert a.size == offa + nea*ESZ_A and b.size == offb + neb*ESZ_B, "sizes"
A = a[offa:].reshape(nea, ESZ_A); B = b[offb:].reshape(neb, ESZ_B)
assert np.array_equal(A[:, 384:], B[:, LB:]), "fixed fields differ"
print("fixed fields (200 B x entries): identical")
# old leaves
lp = A[:, :384].reshape(nea, 128, 3).astype(np.int64)
old = lp[:,:,0] | (lp[:,:,1] << 8) | (lp[:,:,2] << 16)
old = (old ^ (1 << 23)) - (1 << 23)
# new leaves: decode bit fields (chunked to bound memory)
mask = (1 << bits) - 1
CH = 200000; bad = 0
for s in range(0, neb, CH):
    blk = B[s:s+CH, :LB]
    padded = np.concatenate([blk, np.zeros((blk.shape[0], 8), np.uint8)], axis=1)
    new = np.empty((blk.shape[0], 128), np.int64)
    for i in range(128):
        pos = i*bits; byte = pos >> 3; sh = pos & 7
        w = np.zeros(blk.shape[0], np.uint64)
        for k in range(4):
            w |= padded[:, byte+k].astype(np.uint64) << np.uint64(8*k)
        new[:, i] = ((w >> np.uint64(sh)) & np.uint64(mask)).astype(np.int64) + base
    bad += int(np.count_nonzero(new != old[s:s+CH]))
    # unused padding bits in the block must be zero (deterministic output)
    used = 128*bits
    if used < LB*8:
        tail = np.unpackbits(blk, axis=1, bitorder='little')[:, used:]
        assert not tail.any(), "nonzero padding bits"
print(f"leave values compared: {neb*128:,}, mismatches: {bad}")
assert bad == 0
print("OK: v13 table is equivalent to v12")
