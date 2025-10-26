# MAGPIE Word Study Functions - Available C APIs

This document summarizes the C functions available in MAGPIE for building a word study tool (anagram finder, hook finder, extension finder).

## Core KWG (Key Word Graph) Data Structure

The KWG is MAGPIE's lexicon representation. It's a directed acyclic word graph (DAWG) combined with a GADDAG (reverse graph) structure.

**File**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/kwg.h`

### Key KWG Functions

#### 1. **Tree Navigation**
```c
uint32_t kwg_get_next_node_index(const KWG *kwg, 
                                  uint32_t node_index,
                                  MachineLetter letter)
```
- Traverses the KWG from current node following a letter
- Returns 0 if letter not found at this node
- Used for prefix matching and word validation
- **Used for**: Building words character by character

#### 2. **Getting Letter Sets (Hooks and Extensions)**
```c
uint64_t kwg_get_letter_sets(const KWG *kwg, 
                             uint32_t node_index,
                             uint64_t *extension_set)
```
- **Returns**: A bitset of valid letters that can extend the current path forward
- **Output parameter `extension_set`**: A bitset of letters that can extend forward (including invalid word endpoints)
- **Returns**: A bitset of letters that form valid words (accepts/endpoints)
- Each bit represents a letter (bit 0 = blank, bit 1 = A, bit 2 = B, etc.)
- **Used for**: 
  - Finding front hooks (letters to add before a word)
  - Finding back hooks (letters to add after a word)
  - Finding valid extensions

#### 3. **Word Validation**
```c
bool kwg_in_letter_set(const KWG *kwg, MachineLetter letter,
                       uint32_t node_index)
```
- Checks if a letter is in the valid letter set at a node
- Returns true if the letter can form a valid word at this position
- **Used for**: Validating single letter extensions

#### 4. **Root Node Access**
```c
uint32_t kwg_get_root_node_index(const KWG *kwg)
uint32_t kwg_get_dawg_root_node_index(const KWG *kwg)
```
- Get the root nodes for traversal
- GADDAG root for forward/backward word formation
- DAWG root for anagram-like generation
- **kwg_get_root_node_index()**: GADDAG root (for reverse word building)
- **kwg_get_dawg_root_node_index()**: DAWG root (for forward word building)

#### 5. **Helper Functions**
```c
bool kwg_node_is_end(uint32_t node)           // Is this an endpoint?
bool kwg_node_accepts(uint32_t node)          // Does this form a valid word?
uint32_t kwg_node_tile(uint32_t node)         // Get the letter at this node
uint32_t kwg_node_arc_index(uint32_t node)    // Get next arc index
uint32_t kwg_node(const KWG *kwg, uint32_t node_index)  // Get node data
```

## Word Search Implementations Already in MAGPIE

### 1. **Hook Finding Implementation** (in `game.c`)
Located in: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/game.c`, function `game_gen_classic_cross_set()`

The code shows how to find front and back hooks:

```c
// Lines 293-394 show the algorithm:
uint64_t front_hook_set = 0;  // Letters that can be added before the word
uint64_t back_hook_set = 0;   // Letters that can be added after the word

// For finding back hooks (letters to add AFTER left-side word):
if (left_lpath_is_valid) {
    kwg_get_letter_sets(kwg, lnode_index, &leftside_leftx_set);
    const uint32_t s_index = kwg_get_next_node_index(kwg, lnode_index, 
                                                     SEPARATION_MACHINE_LETTER);
    if (s_index != 0) {
        back_hook_set = kwg_get_letter_sets(kwg, s_index, &leftside_rightx_set);
    }
}

// For finding front hooks (letters to add BEFORE right-side word):
if (right_lpath_is_valid) {
    front_hook_set = kwg_get_letter_sets(kwg, right_lnode_index, 
                                        &rightside_leftx_set);
    const uint32_t s_index = kwg_get_next_node_index(kwg, right_lnode_index,
                                                     SEPARATION_MACHINE_LETTER);
    if (s_index != 0) {
        kwg_get_letter_sets(kwg, s_index, &rightside_rightx_set);
    }
}
```

**Key insight**: The SEPARATION_MACHINE_LETTER is used to traverse from forward KWG to backward KWG representation in the combined structure.

### 2. **Word Pruning** (existing function that generates all words)
**File**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/impl/word_prune.h`

```c
void generate_possible_words(const Game *game, 
                            const KWG *override_kwg,
                            DictionaryWordList *possible_word_list)
```
- Generates all possible words that can be formed from current position
- Uses `DictionaryWordList` to collect results
- **Could be used for**: Finding anagrams from a rack

### 3. **Alpha Cross Set Computation** (Wordsmog variant)
**File**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/kwg_alpha.h`

```c
uint64_t kwg_compute_alpha_cross_set(const KWG *kwg, const Rack *rack)
```
- Computes valid letters that can extend words formed from a rack
- Takes a rack of tiles and returns bitset of valid extensions
- **Used for**: Word completion/extension queries

## Supporting Data Structures

### DictionaryWord and DictionaryWordList
**File**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/dictionary_word.h`

```c
typedef struct DictionaryWord {
  MachineLetter word[MAX_KWG_STRING_LENGTH];  // Array of letters
  uint8_t length;                              // Word length
} DictionaryWord;

typedef struct DictionaryWordList {
  DictionaryWord *dictionary_words;
  int count;
  int capacity;
} DictionaryWordList;

// Functions:
DictionaryWordList *dictionary_word_list_create(void);
void dictionary_word_list_add_word(DictionaryWordList *dwl,
                                   const MachineLetter *word, 
                                   int word_length);
int dictionary_word_list_get_count(const DictionaryWordList *dwl);
DictionaryWord *dictionary_word_list_get_word(const DictionaryWordList *dwl,
                                             int index);
void dictionary_word_list_sort(DictionaryWordList *dwl);
void dictionary_word_list_destroy(DictionaryWordList *dwl);
```

### Rack Structure
**File**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/rack.h`

Represents tile rack. Used for anagram generation and extension finding.

## Recommended Approach for Qt Word Study Tool

### 1. **Anagram Finding**
- Use `generate_possible_words()` from `word_prune.h` with an empty board and full rack
- Or manually traverse KWG starting from root with each letter from the rack
- Collect words using `DictionaryWordList`

### 2. **Hook Finding (Front/Back)**
- Start with a word already on the board
- Use `traverse_backwards()` to walk backward through the word in KWG
- Call `kwg_get_letter_sets()` to get valid letters that can extend
- Process with SEPARATION_MACHINE_LETTER for reverse KWG access

### 3. **Extension Finding**
- Given a word prefix or suffix
- Start at appropriate KWG node
- Call `kwg_get_letter_sets()` to get valid continuation letters
- Recursively extend to find all valid words

### 4. **Step-by-step Wrapper Pattern** (Recommended)

Create wrapper functions in `qtpie/magpie_wrapper.c`:

```c
// Find all words that can be made from a rack (anagrams)
void magpie_find_anagrams(const Game *game, 
                         const Rack *rack,
                         DictionaryWordList *results);

// Find hooks that can be added to a word
void magpie_find_hooks(const Game *game, 
                      const MachineLetter *word,
                      int word_length,
                      uint64_t *front_hooks,
                      uint64_t *back_hooks);

// Find all valid extensions of a prefix
void magpie_find_extensions(const Game *game,
                           const MachineLetter *prefix,
                           int prefix_length,
                           DictionaryWordList *results);
```

## Letter Encoding

MachineLetter is a single byte representing a tile:
- 0 = blank
- 1 = A, 2 = B, ..., 26 = Z
- Bit position in sets: bit N represents letter N

Conversion utilities should be in letter_distribution.h for converting between:
- User-facing characters ('A'-'Z', '?')
- MachineLetters (0-26)
- Bitset positions

## Constants and Defines

**File**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/def/kwg_defs.h`

- `SEPARATION_MACHINE_LETTER`: Used to switch between forward/backward KWG traversal
- `MAX_KWG_STRING_LENGTH`: Maximum length of a word in the KWG
- `KWG_NODE_IS_END_FLAG`: Bitmask to check if node is end of arc
- `KWG_NODE_ACCEPTS_FLAG`: Bitmask to check if node forms valid word

## Existing Commands (Reference)

MAGPIE already supports these commands in console mode:
- `generate` - Generates all legal moves from current position
- `simulate` - Runs Monte Carlo simulations
- `infer` - Infers opponent tiles
- `leavegen` - Generates leave values

These don't directly support word study, but show the command/API pattern used.

## Summary of Available Functions

| Task | Function | File | Notes |
|------|----------|------|-------|
| Traverse KWG by letter | `kwg_get_next_node_index()` | kwg.h | Basic tree navigation |
| Get valid hooks/extensions | `kwg_get_letter_sets()` | kwg.h | Returns bitsets |
| Validate word extension | `kwg_in_letter_set()` | kwg.h | Single letter check |
| Find anagrams | `generate_possible_words()` | word_prune.h | Generates all words |
| Get extensions from rack | `kwg_compute_alpha_cross_set()` | kwg_alpha.h | Returns bitset |
| Store word results | `DictionaryWordList` | dictionary_word.h | Collects found words |
| Start traversal (forward) | `kwg_get_dawg_root_node_index()` | kwg.h | DAWG root |
| Start traversal (reverse) | `kwg_get_root_node_index()` | kwg.h | GADDAG root |
