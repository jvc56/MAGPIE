# MAGPIE Word Study Tool - Complete Documentation Index

## Overview

This directory contains comprehensive documentation for implementing word study features (anagrams, hooks, extensions) in a Qt-based graphical frontend using MAGPIE's underlying C library.

## Documentation Files

### Quick Navigation

| File | Size | Purpose | Read If You Want To... |
|------|------|---------|------------------------|
| **README_WORD_STUDY.md** | 8.5 KB | Master overview & navigation | Understand what's available & get started |
| **WORD_STUDY_FUNCTIONS.md** | 9.5 KB | High-level functionality guide | Learn how word study works in MAGPIE |
| **WORD_STUDY_API_REFERENCE.md** | 11 KB | Quick API lookup reference | Find specific functions & signatures |
| **WORD_STUDY_IMPLEMENTATION_GUIDE.md** | 10 KB | Detailed implementation patterns | Write actual code with examples |
| **INDEX.md** | This file | Directory index | Navigate between documents |

## Document Details

### 1. README_WORD_STUDY.md (START HERE)
Master index and quick start guide.

**Contains:**
- Quick start (3 steps)
- Document descriptions
- Key concepts (KWG, MachineLetter, bitsets)
- Three main word study operations (anagrams, hooks, extensions)
- Available functions by category
- Example code patterns
- Integration guide for QtPie
- Common pitfalls and gotchas

**Best for:** Getting oriented and understanding the big picture

### 2. WORD_STUDY_FUNCTIONS.md (LEARN THE DETAILS)
Comprehensive overview of word study functionality in MAGPIE.

**Contains:**
- Core KWG data structure explanation
- Key KWG functions (5 categories):
  1. Tree Navigation
  2. Letter Sets for hooks/extensions
  3. Word Validation
  4. Root Node Access (GADDAG vs DAWG)
  5. Helper Functions
- Existing MAGPIE implementations:
  - Hook finding in game.c (complete algorithm)
  - Word pruning function
  - Alpha cross set computation
- Supporting data structures
- Recommended approach for Qt tool development
- Wrapper pattern for C/C++ integration
- Letter encoding and constants
- Function summary table

**Best for:** Understanding available functionality and design patterns

### 3. WORD_STUDY_API_REFERENCE.md (QUICK LOOKUP)
Complete API reference with function signatures.

**Contains:**
- Essential headers to include (7 files)
- Core KWG Functions (16 functions in tables):
  - Navigation and Traversal (3)
  - Letter Set Queries (2)
  - Node Inspection (5)
  - Alpha Cross Set (3)
- Word List Management (8 functions)
- Rack Operations (8 functions)
- Game and Player Access (3 functions)
- High-Level Functions (2 categories)
- Data Structure definitions with code
- Key Constants
- File locations (absolute paths)
- Code examples:
  - Iterating valid letters from bitset
  - Walking word backward for front hooks
- Performance tips
- C/C++ Integration pattern

**Best for:** Looking up specific functions and their signatures

### 4. WORD_STUDY_IMPLEMENTATION_GUIDE.md (WRITE CODE)
Detailed implementation guide with working code examples.

**Contains:**
- Core data structures overview
- Complete algorithm implementations with code:
  1. Find Anagrams from Rack (recursive code)
  2. Find Front Hooks (backward KWG traversal)
  3. Find Back Hooks (forward KWG traversal)
  4. Find Extensions (prefix continuation search)
- C++ Wrapper Integration Pattern (with code)
- Performance considerations
- Testing approach
- Related source files for reference
- Key functions in table format
- Constants needed

**Best for:** Writing actual implementation code

## Quick Access by Task

### I want to find anagrams
1. Read: WORD_STUDY_FUNCTIONS.md - "Recommended Approach"
2. Reference: WORD_STUDY_IMPLEMENTATION_GUIDE.md - "Algorithm: Find Anagrams"
3. Code location: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/kwg.h`

### I want to find hooks (front/back)
1. Study: WORD_STUDY_FUNCTIONS.md - "Hook Finding Implementation"
2. Reference code: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/game.c:261-396`
3. Implement: WORD_STUDY_IMPLEMENTATION_GUIDE.md - "Find Front/Back Hooks"

### I want to find word extensions
1. Learn: WORD_STUDY_FUNCTIONS.md - "Getting Letter Sets"
2. Reference: WORD_STUDY_API_REFERENCE.md - `kwg_get_letter_sets()`
3. Implement: WORD_STUDY_IMPLEMENTATION_GUIDE.md - "Find Extensions"

### I want to integrate with QtPie
1. Start: README_WORD_STUDY.md - "Integration with QtPie"
2. Pattern: WORD_STUDY_IMPLEMENTATION_GUIDE.md - "C++ Wrapper Integration"
3. Files: `qtpie/magpie_wrapper.h` and `qtpie/magpie_wrapper.c`

## Key Concepts Summary

### KWG (Key Word Graph)
- Combined DAWG (forward) + GADDAG (reverse) structure
- All lexicon lookups go through KWG traversal
- Returns letter sets as uint64_t bitsets

### Three Root Node Types
- `kwg_get_dawg_root_node_index()` - Use for forward word building (anagrams, extensions)
- `kwg_get_root_node_index()` - Use for reverse word building (front hooks)
- SEPARATION_MACHINE_LETTER used to switch between them

### MachineLetter Encoding
```
0 = BLANK
1 = A, 2 = B, ..., 26 = Z
```

### Bitset Format
- uint64_t where each bit represents a letter
- Bit 1 = A, Bit 2 = B, ..., Bit 26 = Z
- Check: `if (bitset & (1ULL << machine_letter))`

## Core Functions at a Glance

| Task | Main Function | Returns |
|------|---------------|---------| 
| Navigate KWG | `kwg_get_next_node_index()` | uint32_t (next node) |
| Get valid letters | `kwg_get_letter_sets()` | uint64_t (bitset of letters) |
| Check validity | `kwg_in_letter_set()` | bool |
| Store words | `DictionaryWordList` | collection |
| Modify rack | `rack_add_letter()` / `rack_take_letter()` | void |

## File Locations (Absolute Paths)

### Core Headers
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/kwg.h` - KWG navigation
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/dictionary_word.h` - Word collection
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/rack.h` - Rack operations
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/game.h` - Game state

### Implementations
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/game.c` - Hook finding (lines 261-396)
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/impl/word_prune.c` - Word generation
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/impl/move_gen.c` - Move generation (uses KWG)

### Constants
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/def/kwg_defs.h` - KWG constants
- `/Users/john/sources/oct21-qtpie/MAGPIE/src/def/letter_distribution_defs.h` - Tile encoding

## Reading Recommendations

### For Quick Understanding (30 minutes)
1. README_WORD_STUDY.md - Key Concepts section
2. WORD_STUDY_FUNCTIONS.md - Overview + Recommended Approach

### For Implementation (2-3 hours)
1. All of README_WORD_STUDY.md
2. WORD_STUDY_IMPLEMENTATION_GUIDE.md - All algorithms
3. WORD_STUDY_API_REFERENCE.md - Reference as needed

### For Deep Dive (full day)
1. All documentation in order
2. Study referenced source files
3. Examine game.c hook finding implementation (261-396)
4. Review word_prune.c implementation

## Implementation Workflow

1. **Understand KWG** (1-2 hours)
   - Read README_WORD_STUDY.md key concepts
   - Study game.c hook finding code
   - Understand DAWG vs GADDAG

2. **Create Wrapper Functions** (2-3 hours)
   - Add functions to magpie_wrapper.c
   - Use patterns from WORD_STUDY_IMPLEMENTATION_GUIDE.md
   - Declare in magpie_wrapper.h

3. **Build Qt UI** (varies)
   - Create word study widgets
   - Call wrapper functions
   - Display results

4. **Test** (1-2 hours)
   - Test wrapper functions
   - Verify results against known word lists
   - Check performance

## Common Questions Answered

**Q: Is there an existing anagram finder function?**
A: No dedicated function, but KWG traversal supports it (see WORD_STUDY_IMPLEMENTATION_GUIDE.md)

**Q: Where is hook finding implemented?**
A: In game.c:261-396 function `game_gen_classic_cross_set()` - this is the reference implementation

**Q: How do I avoid modifying MAGPIE core code?**
A: Create wrapper functions in magpie_wrapper.c following the pattern in WORD_STUDY_IMPLEMENTATION_GUIDE.md

**Q: What's the difference between DAWG and GADDAG?**
A: DAWG is forward (for anagrams/extensions), GADDAG is backward (for front hooks)

**Q: How are valid letters returned?**
A: As uint64_t bitsets where each bit represents a letter (bit 1 = A, etc.)

## Performance Tips

- Cache root node indices (don't lookup repeatedly)
- Use bitwise operations for letter checking (fast)
- Reuse DictionaryWordList (call clear() not create/destroy)
- Clone racks when exploring paths
- Limit recursion depth for UI responsiveness

## Testing

Use MAGPIE's existing test infrastructure:
- Create `test/word_study_test.c`
- Use same build system and runner
- Test against known word lists (CSW, SOWPODS, etc.)

## References

- Original KWG documentation: https://github.com/andy-k/wolges/blob/main/details.txt
- MAGPIE source: `/Users/john/sources/oct21-qtpie/MAGPIE/src/`
- QtPie integration: `qtpie/magpie_wrapper.h/c`

## Statistics

- **Total documentation**: 40+ KB
- **Code examples**: 10+ complete function implementations
- **Tables and references**: 15+
- **Cross-references**: 50+
- **Absolute file paths**: 20+

## Version Information

- **Generated**: October 25, 2025
- **For**: MAGPIE Word Study Tool (QtPie)
- **Based on**: MAGPIE codebase analysis
- **Scope**: src/impl/, src/ent/, src/def/

## Next Steps

1. Read README_WORD_STUDY.md (5 minutes)
2. Read WORD_STUDY_FUNCTIONS.md (15 minutes)
3. Skim WORD_STUDY_API_REFERENCE.md (5 minutes)
4. Study WORD_STUDY_IMPLEMENTATION_GUIDE.md (1-2 hours)
5. Implement wrapper functions (2-3 hours)
6. Build Qt UI (varies)

---

For questions or clarifications, refer to the specific document or examine the referenced source files.
