# MAGPIE Word Study Tool - Documentation Index

This directory contains comprehensive documentation for implementing word study features (anagrams, hooks, extensions) in a Qt-based word study tool using MAGPIE's C library.

## Quick Start

1. **Start here**: Read `WORD_STUDY_FUNCTIONS.md` for an overview of available functionality
2. **Then read**: `WORD_STUDY_API_REFERENCE.md` for quick lookup of specific functions
3. **For implementation**: Use `WORD_STUDY_IMPLEMENTATION_GUIDE.md` with code examples

## Document Descriptions

### WORD_STUDY_FUNCTIONS.md (9.5 KB)
**High-level overview of word study functionality in MAGPIE**

Contents:
- Core KWG (Key Word Graph) data structure explanation
- Key KWG functions (5 categories):
  1. Tree Navigation (`kwg_get_next_node_index`)
  2. Getting Letter Sets for hooks/extensions (`kwg_get_letter_sets`)
  3. Word Validation (`kwg_in_letter_set`)
  4. Root Node Access (GADDAG vs DAWG)
  5. Helper Functions
- Existing implementations in MAGPIE source:
  - Hook finding in `game.c` (complete algorithm)
  - Word pruning function
  - Alpha cross set computation
- Supporting data structures (DictionaryWord, DictionaryWordList, Rack)
- Recommended approach for Qt tool development
- Wrapper pattern for C/C++ integration
- Letter encoding and constants
- Summary table of all available functions

**Best for**: Understanding what's available and how to approach building features

### WORD_STUDY_API_REFERENCE.md (11 KB)
**Complete API reference - like a man page for word study functions**

Contents:
- Essential headers to include (7 files)
- Core KWG Functions (tables with signatures):
  - Navigation and Traversal (3 functions)
  - Letter Set Queries (2 functions)
  - Node Inspection (5 functions)
  - Alpha Cross Set (3 functions)
- Word List Management (8 functions)
- Rack Operations (8 functions)
- Game and Player Access (3 functions)
- High-Level Functions (2 categories)
- Data Structure definitions
- Key Constants
- File locations (absolute paths)
- Code examples:
  - Iterating valid letters from bitset
  - Walking word backward for front hooks
- Performance tips
- C/C++ Integration pattern

**Best for**: Quick lookup when you know what you need

### WORD_STUDY_IMPLEMENTATION_GUIDE.md (10 KB)
**Detailed implementation guide with working code examples**

Contents:
- Core data structures (KWG, MachineLetter, Bitsets)
- Complete algorithm implementations:
  1. **Find Anagrams from Rack** - Full recursive code
  2. **Find Front Hooks** - Backward KWG traversal
  3. **Find Back Hooks** - Forward KWG traversal
  4. **Find Extensions** - Prefix continuation search
- C++ Wrapper Integration Pattern
- Performance considerations
- Testing approach
- Related source files for reference
- Key functions in table format
- Constants needed

**Best for**: Writing actual code - copy, adapt, and use these patterns

## Key Concepts

### KWG Structure
MAGPIE uses a combined DAWG/GADDAG structure:
- **DAWG** (from `kwg_get_dawg_root_node_index()`): Forward word building
- **GADDAG** (from `kwg_get_root_node_index()`): Backward word building + finding hooks

### MachineLetter Encoding
```c
0   = BLANK
1   = A, 2 = B, ..., 26 = Z
```

### Letter Sets (Bitsets)
All valid letters at a position are represented as a `uint64_t` bitset:
- Bit 1 = A, Bit 2 = B, etc.
- Check with: `if (bitset & (1ULL << letter))`

### Three Main Word Study Operations

1. **Anagrams**: Find all words from a rack (empty board)
   - Use: DAWG root + recursive traversal
   - Functions: `kwg_get_dawg_root_node_index()`, `kwg_get_next_node_index()`

2. **Hooks**: Find letters that can be added before/after a word
   - Use: GADDAG root (front) + DAWG root (back)
   - Key function: `kwg_get_letter_sets()` returns both word endpoints and extensions
   - Pattern: Walk word in one direction, get valid additions from other direction

3. **Extensions**: Find words starting with a prefix
   - Use: DAWG root + recursive search
   - Functions: `kwg_get_next_node_index()` to traverse prefix, then explore continuations

## Available Functions by Category

### Core Navigation (in kwg.h)
- `kwg_get_next_node_index()` - Main workhorse for KWG traversal
- `kwg_get_root_node_index()` - GADDAG root (for reverse/hooks)
- `kwg_get_dawg_root_node_index()` - DAWG root (for forward/anagrams)

### Letter Queries (in kwg.h)
- `kwg_get_letter_sets()` - Get valid continuations as bitset
- `kwg_in_letter_set()` - Check if single letter is valid

### Word Management (in dictionary_word.h)
- `dictionary_word_list_create/destroy`
- `dictionary_word_list_add_word()`
- `dictionary_word_list_get_count()`, `get_word()`
- `dictionary_word_list_sort()`, `unique()`

### High-Level (in word_prune.h, kwg_alpha.h)
- `generate_possible_words()` - Generates all words from position
- `kwg_compute_alpha_cross_set()` - Get extensions from rack

## Example Patterns

### Find Back Hooks (letters after word)
```c
uint32_t node = kwg_get_dawg_root_node_index(kwg);
// Walk forward through word
for (int i = 0; i < word_length; i++) {
    node = kwg_get_next_node_index(kwg, node, word[i]);
}
// Get valid continuations
uint64_t extension;
uint64_t hooks = kwg_get_letter_sets(kwg, node, &extension);
```

### Find Front Hooks (letters before word)
```c
uint32_t node = kwg_get_root_node_index(kwg);  // GADDAG
// Walk backward through word
for (int i = word_length - 1; i >= 0; i--) {
    node = kwg_get_next_node_index(kwg, node, word[i]);
}
// Get valid continuations
uint64_t extension;
uint64_t hooks = kwg_get_letter_sets(kwg, node, &extension);
```

## File Locations

All core functionality is in `/Users/john/sources/oct21-qtpie/MAGPIE/src/`:

**KWG and word functions**:
- `ent/kwg.h` - Core KWG navigation
- `ent/kwg_alpha.h` - Alpha cross set functions
- `ent/dictionary_word.h` - Word list management
- `ent/words.h` - Formed words from moves
- `ent/rack.h` - Rack tile operations

**Real-world examples**:
- `ent/game.c:261-396` - Hook finding implementation
- `impl/word_prune.c` - Word generation from position
- `impl/move_gen.c` - Uses KWG for move generation

**Constants and types**:
- `def/kwg_defs.h` - KWG constants
- `def/letter_distribution_defs.h` - Tile encoding

## Integration with QtPie

For QtPie C/C++ integration, create wrapper functions in `qtpie/magpie_wrapper.c`:

```c
// Declare in magpie_wrapper.h
DictionaryWordList *magpie_find_anagrams(const Game *game, const Rack *rack);
uint64_t magpie_get_front_hooks(const Game *game, const MachineLetter *word, int len);
uint64_t magpie_get_back_hooks(const Game *game, const MachineLetter *word, int len);

// Implement in magpie_wrapper.c using patterns from WORD_STUDY_IMPLEMENTATION_GUIDE.md
// Call from C++ without exposing MAGPIE internals
```

This keeps:
- MAGPIE C code unchanged
- Clean C/C++ boundary
- Simple to test

## Next Steps

1. **Learn the basics**: Read `WORD_STUDY_FUNCTIONS.md` section on "Recommended Approach"
2. **Understand KWG traversal**: Study examples in `WORD_STUDY_IMPLEMENTATION_GUIDE.md`
3. **Create wrappers**: Add functions to `qtpie/magpie_wrapper.c`
4. **Build UI**: Create Qt widgets that call wrapper functions
5. **Test**: Use existing MAGPIE test infrastructure

## Common Gotchas

1. **GADDAG vs DAWG**: Use `kwg_get_root_node_index()` for reverse (hooks), `kwg_get_dawg_root_node_index()` for forward
2. **Letter encoding**: MachineLetter 0 = blank, 1 = A, not 0 = A
3. **Bitset operations**: Remember bitset is uint64_t with bits representing letters
4. **Error handling**: `kwg_get_next_node_index()` returns 0 on invalid letter
5. **Node endpoints**: Check `kwg_node_accepts()` to know if position forms valid word

## Performance Considerations

- Cache root node indices
- Use bitwise operations for letter checking
- Reuse DictionaryWordList (clear vs create/destroy)
- Clone racks to avoid complex backtracking
- Limit recursion depth for UI responsiveness

## Testing

Use MAGPIE's existing test infrastructure:
- Create `test/word_study_test.c`
- Use same build system and runner
- Test against known word lists

## References

- Original KWG concept: https://github.com/andy-k/wolges/blob/main/details.txt
- MAGPIE source: `/Users/john/sources/oct21-qtpie/MAGPIE/src/`
- QtPie integration: Follow patterns in `qtpie/magpie_wrapper.h/c`

## Document Statistics

- **WORD_STUDY_FUNCTIONS.md**: 9.5 KB, 150+ lines
- **WORD_STUDY_API_REFERENCE.md**: 11 KB, 200+ lines with tables
- **WORD_STUDY_IMPLEMENTATION_GUIDE.md**: 10 KB, 250+ lines with code
- **This README**: Overview and navigation

Total: 40+ KB of documentation with code examples

---

Generated: October 25, 2025
For: MAGPIE Word Study Tool (QtPie)
Based on: MAGPIE codebase analysis
