# KWG Maker Performance Optimization

## Summary

This PR significantly improves KWG (DAWG/GADDAG) building performance, achieving near-parity with the wolges reference implementation. The optimizations provide substantial speedups for both standalone KWG conversion and endgame solving.

## Benchmark Results

### KWG Conversion (text2kwg CSW24 - 280,887 words)

| Version | Time | Speedup |
|---------|------|---------|
| Before optimization | ~0.54-0.56s | baseline |
| After optimization | ~0.40-0.44s | **1.3x faster** |
| wolges (Rust reference) | ~0.39-0.40s | target |

### Endgame Solver Impact (1000 games, 1-ply)

The KWG builder is used in endgame solving to create pruned dictionaries containing only playable words.

| Component | main branch | optimized | Speedup |
|-----------|-------------|-----------|---------|
| wordprune | 3.08s (3.08ms/game) | 2.55s (2.55ms/game) | **1.21x** (radix sort) |
| kwgmaker | 7.85s (7.85ms/game) | 4.28s (4.28ms/game) | **1.84x** (compact builder) |
| **combined** | 10.93s | 6.83s | **1.60x** |

Average pruned dictionary size: ~6,847 words per endgame position.

## Key Changes

### 1. Compact State-Based KWG Builder (`src/impl/kwg_maker.c`)

Rewrote the KWG builder using a wolges-style approach:

- **Compact 12-byte State struct** (vs 64-byte MutableNode) with sibling chains instead of child arrays
- **Bottom-up construction** with transition stack - states are created and deduplicated immediately when popping
- **Immediate deduplication** during state creation using hash table lookup
- **Memory efficient** - uses linked sibling chains (`next_index`) instead of dynamic child arrays

```c
// New compact state: 12 bytes
typedef struct State {
  uint8_t tile;
  uint8_t accepts;
  uint32_t arc_index;    // First child
  uint32_t next_index;   // Next sibling
} State;
```

### 2. FastStringConverter (`src/ent/letter_distribution.h`)

Added O(1) ASCII lookup table for string-to-machine-letter conversion:

- **256-entry lookup table** for instant ASCII character mapping
- **ASCII fast path** that bypasses UTF-8 handling for English dictionaries
- Falls back to standard conversion for multi-byte characters

```c
typedef struct FastStringConverter {
  MachineLetter ascii_to_ml[256];
  const LetterDistribution *ld;
} FastStringConverter;
```

### 3. LSD Radix Sort (`src/ent/dictionary_word.c`)

Replaced comparison-based sorting with LSD (Least Significant Digit) radix sort:

- **O(n × max_length)** time complexity vs O(n × log(n) × avg_length)
- Optimized for small alphabets (machine letters 0-50)
- Uses double buffering to avoid extra copies

### 4. Lazy Win Percentage Loading (`src/impl/config.c`)

Deferred `win_pct` data loading until actually needed:

- Previously loaded eagerly during config creation
- Now loaded on first use in simulation
- Saves ~0.05s for operations that don't need win percentages (like KWG conversion)

### 5. Bulk KWG Write (`src/ent/kwg.h`)

Optimized file writing on little-endian systems using the compat library's `IS_LITTLE_ENDIAN`:

```c
#if IS_LITTLE_ENDIAN
  // Single fwrite for entire array
  fwrite_or_die(kwg->nodes, sizeof(uint32_t), kwg->number_of_nodes, stream, "kwg nodes");
#else
  // Per-element conversion on big-endian
  for (int i = 0; i < kwg->number_of_nodes; i++) {
    const uint32_t node = htole32(kwg->nodes[i]);
    fwrite_or_die(&node, sizeof(uint32_t), 1, stream, "kwg node");
  }
#endif
```

### 6. Stack-Allocated Word Buffer (`src/impl/convert.c`)

Replaced per-word malloc/free with stack-allocated buffer:

```c
// Before: malloc/free per word
MachineLetter *mls = malloc_or_die(line_length);
// ... use mls ...
free(mls);

// After: stack buffer (words are at most BOARD_DIM letters)
MachineLetter mls[BOARD_DIM + 1];
```

## Files Changed

| File | Changes |
|------|---------|
| `src/impl/kwg_maker.c` | New compact state-based builder (+639 lines) |
| `src/ent/letter_distribution.h` | FastStringConverter for O(1) lookups (+167 lines) |
| `src/ent/dictionary_word.c` | LSD radix sort implementation |
| `src/impl/convert.c` | Stack buffer, FastStringConverter integration |
| `src/impl/config.c` | Lazy win_pct loading |
| `src/ent/kwg.h` | Bulk write optimization |
| `test/letter_distribution_test.c` | FastStringConverter unit tests (+155 lines) |
| `test/win_pct_test.c` | Updated for lazy loading |

## Testing

- All existing tests pass
- Added unit tests for FastStringConverter covering:
  - ASCII lookup table correctness
  - Multiple language alphabets (English, Catalan, Polish)
  - Error handling for invalid characters
  - Edge cases (empty strings, max length)

## Optimization Attempts That Did Not Help

For completeness, the following optimizations were tried but did not improve performance:

1. **xxHash-style hash mixing** - Extra computation overhead outweighed collision reduction
2. **Open addressing hash table** - Comparable to chaining with good hash function
3. **Partial memcpy optimization** - Branch overhead exceeded savings from smaller copies
4. **Pre-allocating word list from file size** - fseek/ftell overhead negated benefits

## Compatibility

- No API changes
- No behavioral changes
- Backward compatible with existing KWG files
- Works on both little-endian and big-endian systems
