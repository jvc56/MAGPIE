# Optimize WMP Generation with Parallel Construction

## Summary

This PR rewrites the WMP (Word Map) generation to use a parallel, sort-based approach that significantly improves performance. The new implementation processes word lengths in parallel, uses efficient radix sorting for BitRack keys, and respects the user's thread configuration.

**Performance improvement: ~2x faster** (from ~1.7s to ~0.87s for CSW24 with 8 threads)

## New Architecture

### Three-Phase Parallel Construction

The WMP is built in three phases, each parallelized across word lengths (2-15):

1. **Phase 1: Word Entries** - Build word map entries and extract unique racks
2. **Phase 2: Blank Entries** - Generate single-blank combinations from unique racks
3. **Phase 3: Double-Blank Entries** - Generate double-blank combinations

Each phase completes before the next begins, allowing buffer reuse between phases.

### Work-Ordered Scheduling

Word lengths are sorted by work (number of words) in descending order before thread launch. This ensures:
- Heavy workloads (7-8 letter words) start first
- Light workloads (2-3 and 14-15 letter words) fill in gaps as cores free up
- Better CPU utilization across all thread counts

### Thread Limiting with Semaphore

A counting semaphore limits concurrent threads to respect the user's `-threads N` setting:
- Threads acquire the semaphore before starting
- Threads release the semaphore when complete
- Main thread can launch new work as soon as any thread finishes

## Key Optimizations

### 1. LSD Radix Sort for 128-bit BitRack Keys

Instead of using comparison-based sorting, the new implementation uses Least-Significant-Digit radix sort:
- 14 passes for English alphabet (27 letters × 4 bits = 108 bits = 14 bytes)
- Alphabet-aware pass count reduces work for smaller alphabets
- Software prefetching in count and scatter phases

### 2. Direct Serialization

Entries are written directly to the final WMPForLength structure:
- No intermediate data structures
- Single pass through sorted pairs builds final output
- Bucket indices computed inline during the merge pass

### 3. Buffer Reuse Across Phases

The `LengthScratchBuffers` structure holds pre-allocated buffers reused across all three phases:
- `scratch1` / `scratch2`: Pair arrays for sorting (WordPair, BlankPair, DoubleBlankPair all fit)
- `bucket_counts`: Reused for bucket counting in each phase
- `unique_racks`: Extracted in Phase 1, used by Phases 2 and 3

### 4. Static Assertions for Safety

Compile-time checks ensure pair types can share buffers:
```c
_Static_assert(sizeof(WordPair) == sizeof(BlankPair), "Pair sizes must match");
_Static_assert(sizeof(WordPair) >= sizeof(DoubleBlankPair), "WordPair must be >= DoubleBlankPair");
```

## API Changes

### `make_wmp_from_words`

```c
// Old signature
WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld);

// New signature
WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld,
                         int num_threads);  // 0 = use all cores
```

### `ConversionArgs`

Added `num_threads` field to pass thread configuration through the convert command.

## Performance Results

Tested on Apple Silicon (M-series) with CSW24 lexicon:

| Threads | Wall Time | CPU Utilization |
|---------|-----------|-----------------|
| 1       | ~1.4s     | 85%             |
| 2       | ~0.97s    | 131%            |
| 4       | ~0.89s    | 177%            |
| 8       | ~0.87s    | 218%            |

The original single-threaded implementation on main takes ~1.7s.

## Files Changed

- `src/impl/wmp_maker.c` - Complete rewrite with parallel construction
- `src/impl/wmp_maker.h` - Updated function signature
- `src/impl/convert.c` - Thread num_threads through conversion
- `src/impl/convert.h` - Add num_threads to ConversionArgs
- `src/impl/config.c` - Fill num_threads in conversion args
- `test/wmp_test.c` - Update for new signature
- `test/wmp_maker_test.c` - Update for new signature
- `convert_lexica.sh` - Use 4 threads for WMP conversion

## Testing

All existing WMP tests pass. The generated WMP files are byte-identical to those produced by the original implementation.
