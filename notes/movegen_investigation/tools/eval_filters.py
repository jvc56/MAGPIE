"""Offline evaluation of anchor filters on dumped through-board WMP anchors.
usage: eval_filters.py WORDS.txt ANCHORS.txt [EXACT_SAMPLE_EVERY]"""
import sys, collections
import numpy as np
words_path, anchors_path = sys.argv[1], sys.argv[2]
exact_every = int(sys.argv[3]) if len(sys.argv) > 3 else 20
ALL = sum(1 << m for m in range(1, 27))
by_len = collections.defaultdict(list)
for w in open(words_path):
    w = w.strip()
    if w: by_len[len(w)].append([ord(c) - 64 for c in w])
W = {L: np.array(v, dtype=np.uint8) for L, v in by_len.items()}
bits = (np.uint32(1) << np.arange(32, dtype=np.uint32))

pos_cache = {}
def posmask(B, L, p):
    k = (B, L, p)
    r = pos_cache.get(k)
    if r is None:
        a = W.get(L)
        if a is None: r = None
        else:
            m = np.all(a[:, p:p+len(B)] == np.array(B, dtype=np.uint8), axis=1)
            sub = a[m]
            if sub.shape[0] == 0: r = None
            else: r = [int(np.bitwise_or.reduce(bits[sub[:, q]])) for q in range(L)]
        pos_cache[k] = r
    return r

union_cache = {}
def unionmask(B, L):
    k = (B, L)
    r = union_cache.get(k)
    if r is None:
        r = 0
        for p in range(0, L - len(B) + 1):
            m = posmask(B, L, p)
            if m is None: continue
            for q in range(L):
                if q < p or q >= p + len(B): r |= m[q]
        union_cache[k] = r
    return r

def rack_bits(rack):
    return ALL if rack[0] > 0 else sum(1 << m for m in range(1, 27) if rack[m] > 0)

def usable(rack, mask):
    return rack[0] + sum(rack[m] for m in range(1, 27) if (mask >> m) & 1)

def exact(L, fixed, allowed, rack, use_counts):
    a = W.get(L)
    if a is None: return False
    ok = np.ones(a.shape[0], dtype=bool)
    for q, ml in fixed.items():
        ok &= a[:, q] == ml
    for q, al in allowed.items():
        ok &= ((bits[a[:, q]] & np.uint32(al)) != 0)
    if not use_counts: return bool(ok.any())
    for row in a[ok]:
        need = collections.Counter(int(row[q]) for q in allowed)
        short = sum(max(0, c - rack[m]) for m, c in need.items())
        if short <= rack[0]: return True
    return False

stats = collections.Counter()
for n, line in enumerate(open(anchors_path)):
    head, letters, cross, rack = line.split('|')
    L, lo, hi, ttp, nblocks, prod, witrej = map(int, head.split())
    letters = list(map(int, letters.split())); cross = [int(x, 16) for x in cross.split()]
    rack = list(map(int, rack.split()))
    rb = rack_bits(rack)
    key = 'prod' if prod else 'unprod'
    stats[key] += 1
    # blocks for the first start col (same blocks for every start col)
    rej = {'union': True, 'pos': True, 'pos_nocross': True, 'exact_sets': True, 'exact_counts': True}
    do_exact = (n % exact_every == 0)
    # current WIT: union masks, anchor-level
    s0 = hi
    blocks = []
    q = 0
    span = letters[s0:s0+L]
    while q < L:
        if span[q]:
            b0 = q
            while q < L and span[q]: q += 1
            blocks.append(tuple(span[b0:q]))
        else: q += 1
    addable = ALL
    for B in blocks: addable &= unionmask(B, L)
    if addable != 0 and usable(rack, addable) >= ttp: rej['union'] = False
    for s in range(lo, hi + 1):
        span = letters[s:s+L]
        fixed = {q: span[q] for q in range(L) if span[q]}
        if len(fixed) != L - ttp: stats['bad_fixed'] += 1
        allowed_x = {q: cross[s+q] & ALL for q in range(L) if not span[q]}
        allowed_r = {q: rb for q in range(L) if not span[q]}
        allowed_xr = {q: allowed_x[q] & rb for q in allowed_x}
        # blocks with offsets
        blks = []; q = 0
        while q < L:
            if span[q]:
                b0 = q
                while q < L and span[q]: q += 1
                blks.append((tuple(span[b0:q]), b0))
            else: q += 1
        for name, base in (('pos', allowed_xr), ('pos_nocross', allowed_r)):
            if not rej[name]: continue
            al = dict(base); dead = False
            for B, p in blks:
                M = posmask(B, L, p)
                if M is None: dead = True; break
                for qq in range(L):
                    if p <= qq < p + len(B): continue
                    if qq in fixed:
                        if not (M[qq] >> fixed[qq]) & 1: dead = True; break
                    else:
                        al[qq] &= M[qq]
                if dead: break
            if not dead:
                if any(v == 0 for v in al.values()): dead = True
                else:
                    u = 0
                    for v in al.values(): u |= v
                    if usable(rack, u) < ttp: dead = True
            if not dead: rej[name] = False
        if do_exact:
            if rej['exact_sets'] and exact(L, fixed, allowed_xr, rack, False): rej['exact_sets'] = False
            if rej['exact_counts'] and exact(L, fixed, allowed_xr, rack, True): rej['exact_counts'] = False
    for name, r in rej.items():
        if name.startswith('exact') and not do_exact: continue
        if r: stats[f'{key}_rej_{name}'] += 1
        if not prod and (r or witrej): stats[f'unprod_rejOrWit_{name}'] += 1
    if not prod and witrej: stats['unprod_witrej'] += 1
    if do_exact: stats[f'{key}_exact_n'] += 1

print(f"anchors: productive {stats['prod']:,}  unproductive {stats['unprod']:,}  (bad_fixed {stats['bad_fixed']})")
for name in ('union', 'pos', 'pos_nocross'):
    print(f"  {name:12s}: rejects {stats['unprod_rej_'+name]/stats['unprod']*100:5.1f}% of unproductive, {stats['prod_rej_'+name]} productive (must be 0)")
for name in ('exact_sets', 'exact_counts'):
    nu, npd = stats['unprod_exact_n'], stats['prod_exact_n']
    print(f"  {name:12s}: rejects {stats['unprod_rej_'+name]/max(nu,1)*100:5.1f}% of unproductive (sample {nu:,}), {stats['prod_rej_'+name]} of {npd:,} productive (must be 0)")
print(f"  real WIT rejects {stats['unprod_witrej']/stats['unprod']*100:5.1f}% of unproductive")
for name in ('pos', 'pos_nocross'):
    print(f"  real WIT or {name}: {stats['unprod_rejOrWit_'+name]/stats['unprod']*100:5.1f}% of unproductive")
print(f"  distinct (block, length, offset) keys touched: {len(pos_cache):,}")
