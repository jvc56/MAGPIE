# KWG Maker Optimization Benchmark Analysis

## Test Configuration
- **Games**: 10,000 endgames
- **Ply depth**: 1
- **Lexicon**: CSW21

## Results Summary

| Component | Main Branch | Optimized Branch | Improvement |
|-----------|-------------|------------------|-------------|
| **Total Time** | 315.8s | 283.3s | **10.3% faster** |
| **Wordprune** | 42.0s (13.3%) | 36.0s (12.7%) | **14.2% faster** |
| **Kwgmaker** | 104.6s (33.1%) | 76.1s (26.9%) | **27.2% faster** |
| **Other (search)** | 169.2s (53.6%) | 171.1s (60.4%) | ~same |

## Analysis

### Endgame Impact
The optimizations provide a **10.3% overall speedup** for endgame solving. While wordprune and kwgmaker represent a minority of total endgame time (~46% on main, ~40% optimized), the improvements to these phases are substantial:

- **Kwgmaker phase**: 27.2% faster (104.6s → 76.1s, saving 28.5s)
- **Wordprune phase**: 14.2% faster (42.0s → 36.0s, saving 6.0s)
- **Combined setup phases**: 23.5% faster (146.6s → 112.1s, saving 34.5s)

The "Other" time (actual alpha-beta search) remains constant as expected since these optimizations only affect the per-endgame setup phase.

### Full Dictionary Impact
For full dictionary builds (tested separately with CSW21):
- **~3x speedup** (10.3s → 3.8s)

The larger improvement for full dictionaries is because kwgmaker dominates the runtime there, whereas in endgames the actual search takes the majority of time.

## Optimizations Applied

1. **LSD Radix Sort** for GADDAG string sorting
   - O(n * max_length) vs O(n log n * avg_length) comparison sort
   - Benefits both wordprune (DictionaryWordList) and kwgmaker (GADDAG strings)

2. **Arena Allocator** for child index arrays in kwgmaker
   - Reduces malloc/free overhead during trie construction
   - Uses linked-list of blocks to maintain pointer stability

## Conclusion

These optimizations meaningfully improve endgame performance (10% faster) while providing dramatic improvements for full dictionary builds (3x faster). The endgame improvement comes from faster setup of the pruned KWG that occurs at the start of each endgame solve.
