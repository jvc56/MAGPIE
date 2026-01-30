# KWG Maker Optimization Notes

This document summarizes the optimization experiments conducted on `kwg_maker.c` for improving KWG (DAWG/GADDAG) creation performance, particularly for the endgame word pruning use case.

## Baseline Timing Analysis

Profiling of `make_kwg_from_words_small` over 75 endgames (~8K words each):

| Phase | Time | Percentage |
|-------|------|------------|
| gaddag_gen | 4.4ms | 36% |
| tree | 3.4ms | 28% |
| merge | 2.2ms | 18% |
| hash | 1.0ms | 8% |
| finalize | 0.3ms | 2% |
| copy | 0.2ms | 2% |

## Successful Optimizations

### 1. LSD Radix Sort for GADDAG String Sorting

**Commit:** `4408a5ef`

Replaced merge sort with LSD (Least Significant Digit) radix sort for sorting GADDAG strings in `dictionary_word_list_sort()`.

**Results:**
- Endgame (~8K words): **47% faster** gaddag_gen phase (8.4ms → 4.4ms)
- Full CSW24 lexicon: **~4% faster** overall KWG creation (1.90s → 1.82s)
- OSPS49 (Polish, 2.8M words): **~3% faster**

**Why it works:** Radix sort is O(n*k) where k is string length, vs O(n log n) for comparison sort. For short strings with limited alphabet, radix sort wins.

### 2. Appropriately Sized Data Structures for Small Dictionaries

**Commit:** Part of `make_kwg_from_words_small` function

Pre-allocate data structures based on word count estimates rather than using large default capacities:
- `MutableNodeList`: capacity = `words_count * 12 + 100`
- `DictionaryWordList` for GADDAG strings: capacity = `words_count * 7`
- Hash table buckets: `estimated_nodes * 2 + 1`

**Why it works:** Reduces memory allocation overhead and improves cache locality for small dictionaries.

## Unsuccessful Optimizations

### 1. Open Addressing Hash Table for Merge Phase

**Attempted:** Replace chained hash table with open addressing (linear probing) for better cache locality.

**Benchmark Results (100 endgames):**
| Approach | Time |
|----------|------|
| Open addressing (2x capacity) | 24.100s |
| Open addressing (4x capacity) | 24.180s |
| Chained hash table | 23.689s |

**Why it didn't help:**
- The chained hash table with pre-allocated `next_indices` array already has good cache locality
- The equality comparison (`mutable_node_equals`) involves recursive subtree comparison which dominates lookup cost
- Open addressing with linear probing suffers from clustering
- The hash table structure itself is not the bottleneck

### 2. Iterative Tree Construction

**Attempted:** Convert recursive `insert_suffix` to iterative version to reduce function call overhead.

**Results:** Made performance **worse**.

**Why it didn't help:**
- The recursive version is well-optimized by the compiler
- The iteration overhead (managing explicit stack/state) exceeded function call overhead
- Cache behavior was worse with the iterative approach

### 3. Incremental Hash Computation During Tree Construction

**Attempted:** Compute node hashes incrementally as tree branches complete, rather than in a separate bottom-up pass.

**Results:** Made performance **worse** (4.8ms vs 4.4ms for hash phase).

**Why it didn't help:**
- Poor cache locality - nodes being hashed are not adjacent in memory
- The separate bottom-up pass has better cache access patterns
- Extra bookkeeping overhead for tracking which nodes are ready

### 4. Increased Inline Capacity for NodeIndexList

**Attempted:** Increase `KWG_NODE_INDEX_LIST_INLINE_CAPACITY` from 2 to 4 or 8 to reduce heap allocations.

**Results:** No improvement, slight regression in some cases.

**Why it didn't help:**
- Larger `MutableNode` struct hurts cache performance
- Most nodes have ≤2 children, so capacity 2 is optimal
- The tradeoff between fewer allocations vs larger struct size favors smaller structs

### 5. Prime Number Hash Table Sizing

**Attempted:** Use prime numbers for hash bucket count to reduce clustering.

**Results:** No measurable difference vs simple `2x + 1` formula.

**Simplified:** Removed `next_prime()` function, using `estimated_nodes * 2 + 1` instead.

## Architectural Insights

### Why the Copy Step Wasn't a Bottleneck

Unlike `wmp_maker.c` which benefited from writing directly to the final format, `kwg_maker.c`'s copy step is only 2% of total time. The DAWG/GADDAG algorithm fundamentally requires:
- Intermediate tree structures for child tracking
- Hash-based merging for node deduplication
- These cannot be eliminated without changing the algorithm

### Performance Bounds

The remaining performance is bounded by fundamental algorithmic constraints:
- **GADDAG generation (36%)**: Must generate L strings per word of length L
- **Tree building (28%)**: Must traverse and link all nodes
- **Merge (18%)**: Must compare nodes for deduplication

Further optimization would require algorithmic changes to the DAWG/GADDAG construction approach itself.

## Summary of Changes

| Change | Status | Impact |
|--------|--------|--------|
| LSD radix sort | ✅ Committed | 47% faster gaddag_gen |
| Sized data structures | ✅ Committed | Reduced allocations |
| Simplified hash sizing | ✅ Committed | Code cleanup |
| Open addressing hash | ❌ Reverted | Slower |
| Iterative tree build | ❌ Reverted | Slower |
| Incremental hashing | ❌ Reverted | Slower |
| Inline capacity changes | ❌ Reverted | No improvement |
| MutableNode size reduction | ❌ Reverted | Slower |

## Unsuccessful Optimization: MutableNode Size Reduction

**Attempted:** Reduce MutableNode struct from 64 bytes to smaller sizes for better cache utilization.

**Approaches tried:**

1. **48 bytes with flags** - Combined accepts/is_end into uint8_t flags, used uint32_t merged_into_index instead of pointer
   - Result: ~2-3% SLOWER

2. **48 bytes with bools** - Same as above but kept bool accepts/is_end
   - Result: ~2-3% SLOWER

3. **56 bytes** - Only changed NodeIndexList count/capacity from size_t to uint32_t
   - Result: ~5-7% SLOWER

**Why it didn't help:**
- The 64-byte struct is a power of 2, which may have optimal alignment properties on x86-64
- Using uint32_t instead of size_t may cause extra zero-extension operations on 64-bit CPUs
- The original struct layout was already well-optimized for the specific access patterns
- Field reordering disrupted cache-friendly access patterns

**Conclusion:** The MutableNode struct is already well-optimized at 64 bytes. Do not attempt to reduce its size.

## Remaining Ideas (Not Tried)

These ideas were considered but not implemented. They target the main bottlenecks.

### GADDAG Generation (36%)

1. **Parallel string generation**: Generate GADDAG strings for different words in parallel. Risk: thread overhead may exceed benefit for ~8K words.

2. **In-place string reversal**: Avoid copying strings during GADDAG generation by working with indices. Risk: may complicate code without significant benefit since radix sort already dominates.

3. **Fused generation + sorting**: Generate strings in a way that's partially sorted. Risk: complex to implement correctly.

### Tree Building (28%)

1. **Memory pool allocator**: Replace individual `malloc` calls with a pre-allocated memory pool for nodes. Could reduce allocation overhead.

2. **Batch child insertion**: Instead of adding children one at a time, batch insertions when multiple children are known. Risk: requires algorithm changes.

### Merge Phase (18%)

1. **Bloom filter pre-check**: Use a bloom filter to quickly reject non-matching nodes before expensive `mutable_node_equals` comparison. Risk: bloom filter overhead may exceed savings.

2. **Better hash function**: Current hash uses XOR with rotation. Could try xxHash or other fast hash functions to reduce collisions.

3. **Hash comparison short-circuit**: Compare `hash_with_node` values before full subtree comparison. Already partially done but could be more aggressive.

### Cross-Cutting

1. **Profile-guided optimization (PGO)**: Build with profiling, run benchmarks, rebuild with profile data. Could help compiler optimize hot paths.

2. **SIMD string comparison**: Use SIMD instructions for string matching in `insert_suffix`. Risk: strings are short (~6 chars), SIMD setup overhead may dominate.

## Test Commands

```bash
# Run kwgmaker tests
./bin/magpie_test kwgmaker

# Run word prune tests
./bin/magpie_test wordprune

# Benchmark endgame performance (includes KWG creation)
make BUILD=release -j8
./bin/magpie_test benchend
```
