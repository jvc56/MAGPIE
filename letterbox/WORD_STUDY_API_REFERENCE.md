# MAGPIE Word Study API Reference

Quick reference for C functions and data structures needed to build word study tools.

## Essential Headers to Include

```c
#include "src/ent/kwg.h"               // KWG traversal functions
#include "src/ent/kwg_alpha.h"          // Alpha cross set computation
#include "src/ent/dictionary_word.h"    // Word collection/storage
#include "src/ent/rack.h"               // Tile rack operations
#include "src/ent/game.h"               // Game state
#include "src/ent/player.h"             // Player state (contains KWG)
#include "src/impl/word_prune.h"        // Word generation from position
#include "src/def/kwg_defs.h"           // KWG constants
```

## Core KWG Functions

### Navigation and Traversal

| Function | Signature | Purpose |
|----------|-----------|---------|
| `kwg_get_next_node_index` | `uint32_t kwg_get_next_node_index(const KWG *kwg, uint32_t node_index, MachineLetter letter)` | Move to next node by letter; returns 0 if invalid |
| `kwg_get_root_node_index` | `uint32_t kwg_get_root_node_index(const KWG *kwg)` | Get GADDAG root (for reverse/hook finding) |
| `kwg_get_dawg_root_node_index` | `uint32_t kwg_get_dawg_root_node_index(const KWG *kwg)` | Get DAWG root (for forward word building) |

### Letter Set Queries

| Function | Signature | Purpose |
|----------|-----------|---------|
| `kwg_get_letter_sets` | `uint64_t kwg_get_letter_sets(const KWG *kwg, uint32_t node_index, uint64_t *extension_set)` | Get valid continuation letters; returns valid word endpoints, sets extension_set |
| `kwg_in_letter_set` | `bool kwg_in_letter_set(const KWG *kwg, MachineLetter letter, uint32_t node_index)` | Check if single letter is valid at node |

### Node Inspection

| Function | Signature | Purpose |
|----------|-----------|---------|
| `kwg_node` | `uint32_t kwg_node(const KWG *kwg, uint32_t node_index)` | Get node data at index |
| `kwg_node_tile` | `uint32_t kwg_node_tile(uint32_t node)` | Extract letter from node |
| `kwg_node_accepts` | `bool kwg_node_accepts(uint32_t node)` | Check if this forms a valid word |
| `kwg_node_is_end` | `bool kwg_node_is_end(uint32_t node)` | Check if end of arc list |
| `kwg_node_arc_index` | `uint32_t kwg_node_arc_index(uint32_t node)` | Get next arc index |

### Alpha Cross Set (Wordsmog Variant)

| Function | Signature | Purpose |
|----------|-----------|---------|
| `kwg_compute_alpha_cross_set` | `uint64_t kwg_compute_alpha_cross_set(const KWG *kwg, const Rack *rack)` | Get valid letters that can extend words from rack |
| `kwg_accepts_alpha` | `bool kwg_accepts_alpha(const KWG *kwg, const Rack *rack)` | Check if rack forms valid word |
| `kwg_accepts_alpha_with_blanks` | `bool kwg_accepts_alpha_with_blanks(const KWG *kwg, const Rack *rack)` | Check with blank handling |

## Word List Management

### Create/Destroy

| Function | Signature | Purpose |
|----------|-----------|---------|
| `dictionary_word_list_create` | `DictionaryWordList *dictionary_word_list_create(void)` | Create empty word list |
| `dictionary_word_list_create_with_capacity` | `DictionaryWordList *dictionary_word_list_create_with_capacity(int capacity)` | Create with initial capacity |
| `dictionary_word_list_destroy` | `void dictionary_word_list_destroy(DictionaryWordList *dwl)` | Free word list |
| `dictionary_word_list_clear` | `void dictionary_word_list_clear(DictionaryWordList *dwl)` | Clear but reuse memory |

### Populate/Query

| Function | Signature | Purpose |
|----------|-----------|---------|
| `dictionary_word_list_add_word` | `void dictionary_word_list_add_word(DictionaryWordList *dwl, const MachineLetter *word, int word_length)` | Add word to list |
| `dictionary_word_list_get_count` | `int dictionary_word_list_get_count(const DictionaryWordList *dwl)` | Get number of words |
| `dictionary_word_list_get_word` | `DictionaryWord *dictionary_word_list_get_word(const DictionaryWordList *dwl, int index)` | Get word at index |
| `dictionary_word_list_sort` | `void dictionary_word_list_sort(DictionaryWordList *dwl)` | Sort word list |
| `dictionary_word_list_unique` | `void dictionary_word_list_unique(DictionaryWordList *sorted, DictionaryWordList *unique)` | Remove duplicates |

## Rack Operations

### Query

| Function | Signature | Purpose |
|----------|-----------|---------|
| `rack_get_letter` | `int8_t rack_get_letter(const Rack *rack, MachineLetter letter)` | Get count of letter in rack |
| `rack_get_dist_size` | `uint16_t rack_get_dist_size(const Rack *rack)` | Get alphabet size |

### Modify

| Function | Signature | Purpose |
|----------|-----------|---------|
| `rack_add_letter` | `void rack_add_letter(Rack *rack, MachineLetter letter)` | Add letter to rack |
| `rack_take_letter` | `void rack_take_letter(Rack *rack, MachineLetter letter)` | Remove letter from rack |
| `rack_reset` | `void rack_reset(Rack *rack)` | Clear all tiles |
| `rack_copy` | `void rack_copy(Rack *dst, const Rack *src)` | Copy rack |

## Game and Player Access

| Function | Signature | Purpose |
|----------|-----------|---------|
| `game_get_player` | `Player *game_get_player(const Game *game, int player_index)` | Get player 0 or 1 |
| `player_get_kwg` | `const KWG *player_get_kwg(const Player *player)` | Get KWG from player |
| `game_get_ld` | `const LetterDistribution *game_get_ld(const Game *game)` | Get letter distribution |

## High-Level Functions

### Word Generation from Position

| Function | Signature | Purpose |
|----------|-----------|---------|
| `generate_possible_words` | `void generate_possible_words(const Game *game, const KWG *override_kwg, DictionaryWordList *possible_word_list)` | Generate all legal words from current board position |

### Formed Words Validation

| Function | Signature | Purpose |
|----------|-----------|---------|
| `formed_words_create` | `FormedWords *formed_words_create(Board *board, const Move *move)` | Create word list from move |
| `formed_words_populate_validities` | `void formed_words_populate_validities(const KWG *kwg, FormedWords *ws, bool is_wordsmog)` | Validate formed words |
| `formed_words_get_word` | `const MachineLetter *formed_words_get_word(const FormedWords *fw, int word_index)` | Get word letter array |
| `formed_words_get_word_length` | `int formed_words_get_word_length(const FormedWords *fw, int word_index)` | Get word length |

## Data Structures

### MachineLetter
```c
typedef uint8_t MachineLetter;
// 0 = BLANK, 1 = A, 2 = B, ..., 26 = Z
```

### DictionaryWord
```c
typedef struct DictionaryWord {
  MachineLetter word[MAX_KWG_STRING_LENGTH];
  uint8_t length;
} DictionaryWord;
```

### DictionaryWordList
```c
typedef struct DictionaryWordList {
  DictionaryWord *dictionary_words;
  int count;
  int capacity;
} DictionaryWordList;
```

### Bitset Format
```c
// Each uint64_t bitset uses bits to represent letters
// Bit 0 = BLANK
// Bit 1 = A (letter 1 in MachineLetter)
// Bit 2 = B (letter 2 in MachineLetter)
// ...
// Bit 26 = Z (letter 26 in MachineLetter)

// Check if letter is in set:
if (letter_set & (1ULL << machine_letter)) {
    // letter is valid
}

// Get all valid letters:
for (MachineLetter letter = 0; letter <= 26; letter++) {
    if (letter_set & (1ULL << letter)) {
        // process letter
    }
}
```

## Key Constants

| Constant | File | Purpose |
|----------|------|---------|
| `SEPARATION_MACHINE_LETTER` | kwg_defs.h | Marker for DAWG/GADDAG transition |
| `MAX_KWG_STRING_LENGTH` | kwg_defs.h | Maximum word length |
| `KWG_NODE_IS_END_FLAG` | kwg_defs.h | Bitmask for arc end detection |
| `KWG_NODE_ACCEPTS_FLAG` | kwg_defs.h | Bitmask for word endpoint detection |

## File Locations (Absolute Paths)

```
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/kwg.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/kwg_alpha.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/dictionary_word.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/rack.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/game.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/player.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/words.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/letter_distribution.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/impl/word_prune.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/def/kwg_defs.h
/Users/john/sources/oct21-qtpie/MAGPIE/src/def/letter_distribution_defs.h
```

## Example: Iterating Valid Letters from Bitset

```c
uint64_t valid_letters;
uint64_t extension_set;
kwg_get_letter_sets(kwg, node_index, &extension_set);

// valid_letters contains hooks that form words
// extension_set contains letters that can extend further

for (MachineLetter letter = 1; letter <= 26; letter++) {
    uint64_t bit = 1ULL << letter;
    
    if (valid_letters & bit) {
        // This letter creates a valid word (valid_letters)
        printf("Valid word with letter %d\n", letter);
    }
    
    if (extension_set & bit) {
        // This letter can extend further (but may not form word)
        printf("Can extend with letter %d\n", letter);
    }
}
```

## Example: Walking a Word Backward (for front hooks)

```c
const KWG *kwg = player_get_kwg(game_get_player(game, 0));
MachineLetter word[] = {3, 1, 20};  // "CAT" (C=3, A=1, T=20)
int word_length = 3;

uint32_t node_index = kwg_get_root_node_index(kwg);  // GADDAG root

// Walk backward through word
for (int i = word_length - 1; i >= 0; i--) {
    node_index = kwg_get_next_node_index(kwg, node_index, word[i]);
    if (node_index == 0) {
        printf("Word not in dictionary\n");
        return;
    }
}

// Get front hooks
uint64_t extension_set;
uint64_t front_hooks = kwg_get_letter_sets(kwg, node_index, &extension_set);
printf("Front hooks: 0x%016llx\n", front_hooks);
```

## Performance Tips

1. **Cache root node indices** - Don't call `kwg_get_root_node_index()` repeatedly
2. **Use bitwise operations** - They're fast for checking letter validity
3. **Reuse DictionaryWordList** - Call clear() rather than create/destroy repeatedly
4. **Rack copies** - Clone racks when exploring multiple paths to avoid complex backtracking
5. **Limit recursion depth** - For UI responsiveness, can limit word length queries

## Related Source Code Examples

- **Hook finding code**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/ent/game.c:261-396` (function `game_gen_classic_cross_set`)
- **Word generation**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/impl/word_prune.c`
- **Move generation uses KWG**: `/Users/john/sources/oct21-qtpie/MAGPIE/src/impl/move_gen.c`

## C/C++ Integration (QtPie Pattern)

Create wrapper functions in `qtpie/magpie_wrapper.c` to provide clean C interfaces:

```c
// magpie_wrapper.h
typedef struct {
    MachineLetter *words;
    int word_count;
    int *word_lengths;
} WordStudyResult;

WordStudyResult *magpie_get_hooks(const Game *game,
                                  const MachineLetter *word,
                                  int word_length);
void magpie_wordstudyresult_destroy(WordStudyResult *result);

// Then call from C++ without exposing KWG internals
```

This keeps MAGPIE C code unchanged and provides a clean API for C++ code.
