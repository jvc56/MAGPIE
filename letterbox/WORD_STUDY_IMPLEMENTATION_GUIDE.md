# MAGPIE Word Study Tool - Implementation Guide

This document provides detailed examples and code patterns for implementing anagram, hook, and extension finding in a Qt word study tool.

## Core Data Structures to Use

### 1. KWG (Key Word Graph)
The lexicon is stored in a combined DAWG/GADDAG structure:
- **DAWG** (node index from `kwg_get_dawg_root_node_index()`): Forward word building
- **GADDAG** (node index from `kwg_get_root_node_index()`): Backward word building + extensions

### 2. MachineLetter Encoding
```c
// Single byte encoding (MachineLetter is uint8_t)
0   = BLANK
1   = A
2   = B
... 
26  = Z
```

### 3. Letter Sets (Bitsets)
```c
// Each valid_letters bitset is a uint64_t
// Bit N represents whether letter N is valid
// Bit 0 = blank, bit 1 = A, bit 2 = B, ..., bit 26 = Z

// Example: Check if 'A' (MachineLetter 1) is in a set
uint64_t valid_set = ...; // from kwg_get_letter_sets()
if (valid_set & (1ULL << 1)) {
    // A is a valid letter here
}

// Iterate all valid letters:
for (MachineLetter letter = 1; letter < 27; letter++) {
    if (valid_set & (1ULL << letter)) {
        // letter is valid
    }
}
```

## Algorithm: Find Anagrams from a Rack

```c
// File: qtpie/magpie_wrapper.c (pseudocode)

void find_anagrams(const Game *game, 
                  const Rack *rack,
                  DictionaryWordList *results) {
    const KWG *kwg = player_get_kwg(game_get_player(game, 0));
    const LetterDistribution *ld = game_get_ld(game);
    
    // Start from DAWG root (forward word building)
    uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
    
    // Recursive function to explore all paths
    find_anagrams_recursive(kwg, rack, node_index, 
                           NULL, 0, results);
}

// Recursive helper:
static void find_anagrams_recursive(
    const KWG *kwg,
    const Rack *rack,
    uint32_t node_index,
    MachineLetter *current_word,
    int word_length,
    DictionaryWordList *results) {
    
    const uint32_t node = kwg_node(kwg, node_index);
    
    // If this node represents a valid word, add to results
    if (kwg_node_accepts(node)) {
        dictionary_word_list_add_word(results, current_word, word_length);
    }
    
    // If end of this arc, stop recursion
    if (kwg_node_is_end(node)) {
        return;
    }
    
    // Try each letter in the rack
    for (MachineLetter letter = 1; letter < 27; letter++) {
        if (rack_get_letter(rack, letter) > 0) {
            // Try this letter
            uint32_t next_node = kwg_get_next_node_index(kwg, node_index, letter);
            
            if (next_node != 0) {
                // Letter exists in dictionary
                rack_take_letter(rack, letter);
                current_word[word_length] = letter;
                
                find_anagrams_recursive(kwg, rack, next_node,
                                      current_word, word_length + 1,
                                      results);
                
                rack_add_letter(rack, letter);
            }
        }
    }
}
```

## Algorithm: Find Front Hooks

Front hooks are letters that can be added before a word.

```c
// Given: a word like "CAT"
// Find: which letters can go before CAT (B in BCAT, S in SCAT, etc.)

uint64_t find_front_hooks(const Game *game,
                         const MachineLetter *word,
                         int word_length) {
    const KWG *kwg = player_get_kwg(game_get_player(game, 0));
    
    // Start from GADDAG root (reverse building)
    uint32_t node_index = kwg_get_root_node_index(kwg);
    
    // Traverse the word BACKWARDS through the GADDAG
    // to find what letters can come before it
    for (int i = word_length - 1; i >= 0; i--) {
        node_index = kwg_get_next_node_index(kwg, node_index, word[i]);
        if (node_index == 0) {
            return 0;  // Word not in dictionary
        }
    }
    
    // At this point, we're at the end of CAT in reverse
    // Now get all letters that can come AFTER it in the GADDAG
    // (which means BEFORE it in normal reading)
    uint64_t extension_set;
    uint64_t front_hooks = kwg_get_letter_sets(kwg, node_index, &extension_set);
    
    return front_hooks;
}
```

## Algorithm: Find Back Hooks

Back hooks are letters that can be added after a word.

```c
// Given: a word like "CAT"
// Find: which letters can go after CAT (S in CATS, A in CATA, etc.)

uint64_t find_back_hooks(const Game *game,
                        const MachineLetter *word,
                        int word_length) {
    const KWG *kwg = player_get_kwg(game_get_player(game, 0));
    
    // Start from DAWG root (forward building)
    uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
    
    // Traverse the word FORWARDS through the DAWG
    for (int i = 0; i < word_length; i++) {
        node_index = kwg_get_next_node_index(kwg, node_index, word[i]);
        if (node_index == 0) {
            return 0;  // Word not in dictionary
        }
    }
    
    // At this point, we're at the end of CAT in forward
    // Now get all letters that can come AFTER it
    uint64_t extension_set;
    uint64_t back_hooks = kwg_get_letter_sets(kwg, node_index, &extension_set);
    
    return back_hooks;
}
```

## Algorithm: Find Extensions (Word Continuations)

Given a prefix, find all valid words that start with that prefix.

```c
// Given: "CAT"
// Find: "CATS", "CATER", "CATBIRD", etc.

void find_extensions(const Game *game,
                    const MachineLetter *prefix,
                    int prefix_length,
                    DictionaryWordList *results) {
    const KWG *kwg = player_get_kwg(game_get_player(game, 0));
    
    // Start from DAWG root
    uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
    
    // Traverse to end of prefix
    for (int i = 0; i < prefix_length; i++) {
        node_index = kwg_get_next_node_index(kwg, node_index, prefix[i]);
        if (node_index == 0) {
            return;  // Prefix not in dictionary
        }
    }
    
    // Now recursively find all valid continuations
    find_extensions_recursive(kwg, node_index,
                            prefix, prefix_length,
                            results);
}

static void find_extensions_recursive(
    const KWG *kwg,
    uint32_t node_index,
    MachineLetter *current_word,
    int word_length,
    DictionaryWordList *results) {
    
    const uint32_t node = kwg_node(kwg, node_index);
    
    // If this is a valid word, add it
    if (kwg_node_accepts(node)) {
        dictionary_word_list_add_word(results, current_word, word_length);
    }
    
    // If end of arc, stop
    if (kwg_node_is_end(node)) {
        return;
    }
    
    // Try all letters that can come next
    uint32_t next_index = node_index;
    while (1) {
        const uint32_t n = kwg_node(kwg, next_index);
        const MachineLetter letter = kwg_node_tile(n);
        
        current_word[word_length] = letter;
        find_extensions_recursive(
            kwg,
            kwg_node_arc_index(n),
            current_word,
            word_length + 1,
            results);
        
        if (kwg_node_is_end(n)) {
            break;
        }
        next_index++;
    }
}
```

## C++ Wrapper Integration Pattern

For QtPie, create wrapper functions in `magpie_wrapper.c`:

```c
// magpie_wrapper.h
typedef struct {
    uint8_t *letters;
    int count;
} WordList;

WordList *magpie_find_anagrams_c(const Rack *rack, 
                                 const KWG *kwg);
void magpie_wordlist_destroy(WordList *wl);

// magpie_wrapper.c
WordList *magpie_find_anagrams_c(const Rack *rack, 
                                 const KWG *kwg) {
    DictionaryWordList *dwl = dictionary_word_list_create();
    
    // ... call find_anagrams_recursive() ...
    
    WordList *result = malloc(sizeof(WordList));
    result->count = dictionary_word_list_get_count(dwl);
    result->letters = malloc(...);
    
    // Copy results
    for (int i = 0; i < result->count; i++) {
        DictionaryWord *dw = dictionary_word_list_get_word(dwl, i);
        // Copy to result->letters...
    }
    
    dictionary_word_list_destroy(dwl);
    return result;
}
```

Then in C++:
```cpp
// board_view.cpp or word_study_widget.cpp
WordList *results = magpie_find_anagrams_c(rack, kwg);
for (int i = 0; i < results->count; i++) {
    // Display word
}
magpie_wordlist_destroy(results);
```

## Performance Considerations

1. **Caching KWG Roots**: Cache the root node indices to avoid repeated lookups
2. **Letter Set Bitwise Operations**: Use bitwise AND/OR for efficiency
3. **Rack Cloning**: Make copies of racks when exploring paths to avoid complex backtracking
4. **Thread Safety**: Ensure thread-local storage if using multiple threads (see MAGPIE's cached_gens pattern)

## Testing

Use existing MAGPIE test infrastructure:
- Create test file: `test/word_study_test.c`
- Use same build system and test runner
- Test against known anagrams and hooks from reference materials

## Related Files for Reference

- **Hook finding example**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/game.c` lines 261-396
- **Word generation example**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/impl/word_prune.c`
- **Rack manipulation**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/rack.h`
- **KWG traversal**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/kwg.h` and `kwg_alpha.h`

## Key Functions Available

| Function | Returns | Purpose |
|----------|---------|---------|
| `kwg_get_next_node_index()` | uint32_t | Navigate to next node by letter |
| `kwg_get_letter_sets()` | uint64_t | Get valid continuation letters (bitset) |
| `kwg_node_accepts()` | bool | Check if current position forms valid word |
| `kwg_node_is_end()` | bool | Check if end of arc list |
| `kwg_node_tile()` | uint32_t | Get letter at current node |
| `dictionary_word_list_*` | various | Collect and manage word results |
| `rack_get_letter()` | int | Get count of letter in rack |
| `rack_add_letter()` | void | Add letter to rack |
| `rack_take_letter()` | void | Remove letter from rack |

## Constants Needed

Include these header files for constants:
```c
#include "src/def/kwg_defs.h"           // KWG constants
#include "src/def/letter_distribution_defs.h"  // Tile encoding
#include "src/def/board_defs.h"         // Board dimensions
```
