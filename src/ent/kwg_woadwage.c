#include "kwg_woadwage.h"

#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "kwg.h"
#include <stdbool.h>
#include <stdint.h>

// Everything here reads the GADDAG alone, since the endgame's pruned KWGs
// have no DAWG. A path without a separator spells a word backward from its
// last letter; a word's first letter, the separator and the rest spell it
// forward.

// Follows the arc for ml out of the list at node_index. Returns whether the
// list has one; if it does, *next_node_index is the list it leads to (0 if
// none) and *accepts whether it ends a word.
static inline bool kwg_woadwage_follow(const KWG *kwg, uint32_t node_index,
                                       MachineLetter ml,
                                       uint32_t *next_node_index,
                                       bool *accepts) {
  if (node_index == 0) {
    return false;
  }
  const uint64_t list_tiles = kwg_get_sibling_tiles(kwg)[node_index];
  const uint64_t tile_bit = (uint64_t)1 << ml;
  if ((list_tiles & tile_bit) == 0) {
    return false;
  }
  const uint32_t node = kwg_list_node(kwg, node_index, list_tiles, tile_bit);
  *next_node_index = kwg_node_arc_index(node);
  *accepts = kwg_node_accepts(node);
  return true;
}

// A word read forward from the GADDAG root.
typedef struct KwgWoadwageForward {
  uint32_t node_index;
  bool read_first_letter;
} KwgWoadwageForward;

static inline KwgWoadwageForward kwg_woadwage_forward(const KWG *kwg) {
  return (KwgWoadwageForward){.node_index = kwg_get_root_node_index(kwg),
                              .read_first_letter = false};
}

// Reads ml next. Returns whether some word goes on with it, and if one does,
// *accepts is whether the letters read are a word.
static inline bool kwg_woadwage_read_forward(const KWG *kwg,
                                             KwgWoadwageForward *forward,
                                             MachineLetter ml, bool *accepts) {
  uint32_t next_node_index;
  if (!kwg_woadwage_follow(kwg, forward->node_index, ml, &next_node_index,
                           accepts)) {
    return false;
  }
  if (!forward->read_first_letter) {
    forward->read_first_letter = true;
    next_node_index = next_node_index == 0
                          ? 0
                          : kwg_get_next_node_index(kwg, next_node_index,
                                                    SEPARATION_MACHINE_LETTER);
  }
  forward->node_index = next_node_index;
  return true;
}

// Whether letters[0..length) is a word of the lexicon.
static bool kwg_woadwage_is_word(const KWG *kwg, const MachineLetter *letters,
                                 int length) {
  KwgWoadwageForward forward = kwg_woadwage_forward(kwg);
  bool accepts = false;
  for (int letter_idx = 0; letter_idx < length; letter_idx++) {
    if (!kwg_woadwage_read_forward(kwg, &forward, letters[letter_idx],
                                   &accepts)) {
      return false;
    }
  }
  return accepts;
}

bool kwg_accepts_woadwage(const KWG *kwg, const MachineLetter *letters,
                          int length) {
  // One walk along the letters: wherever a word ends before the last letter,
  // the rest may be the second word.
  KwgWoadwageForward forward = kwg_woadwage_forward(kwg);
  bool accepts = false;
  for (int letter_idx = 0; letter_idx < length; letter_idx++) {
    if (!kwg_woadwage_read_forward(kwg, &forward, letters[letter_idx],
                                   &accepts)) {
      return false;
    }
    if (accepts && letter_idx < length - 1 &&
        kwg_woadwage_is_word(kwg, letters + letter_idx + 1,
                             length - letter_idx - 1)) {
      return true;
    }
  }
  return accepts;
}

// The letters x for which before, x and after make one word of the lexicon,
// read as classic cross sets are: the word backward from its last letter,
// after's letters, x, then before's.
static uint64_t kwg_woadwage_word_cross_set(const KWG *kwg,
                                            const MachineLetter *before,
                                            int before_length,
                                            const MachineLetter *after,
                                            int after_length) {
  uint32_t node_index = kwg_get_root_node_index(kwg);
  bool accepts;
  for (int letter_idx = after_length - 1; letter_idx >= 0; letter_idx--) {
    if (!kwg_woadwage_follow(kwg, node_index, after[letter_idx], &node_index,
                             &accepts)) {
      return 0;
    }
  }
  if (node_index == 0) {
    return 0;
  }
  uint64_t extension_set;
  const uint64_t ending_set =
      kwg_get_letter_sets(kwg, node_index, &extension_set);
  if (before_length == 0) {
    return ending_set;
  }
  uint64_t cross_set = 0;
  const uint64_t list_tiles = kwg_get_sibling_tiles(kwg)[node_index];
  for (uint64_t remaining = extension_set; remaining != 0;
       remaining &= remaining - 1) {
    const uint64_t tile_bit = remaining & (~remaining + 1);
    uint32_t before_node_index = kwg_node_arc_index(
        kwg_list_node(kwg, node_index, list_tiles, tile_bit));
    accepts = false;
    int letter_idx = before_length - 1;
    while (letter_idx >= 0 &&
           kwg_woadwage_follow(kwg, before_node_index, before[letter_idx],
                               &before_node_index, &accepts)) {
      letter_idx--;
    }
    if (letter_idx < 0 && accepts) {
      cross_set |= tile_bit;
    }
  }
  return cross_set;
}

uint64_t kwg_compute_woadwage_cross_set(const KWG *kwg,
                                        const MachineLetter *before,
                                        int before_length,
                                        const MachineLetter *after,
                                        int after_length) {
  // The square's letter is in one word of the lexicon,
  uint64_t cross_set = kwg_woadwage_word_cross_set(kwg, before, before_length,
                                                   after, after_length);
  // or in the second of two, after a first word that begins the letters
  // before it,
  KwgWoadwageForward forward = kwg_woadwage_forward(kwg);
  bool accepts;
  for (int letter_idx = 0; letter_idx < before_length; letter_idx++) {
    if (!kwg_woadwage_read_forward(kwg, &forward, before[letter_idx],
                                   &accepts)) {
      break;
    }
    if (accepts) {
      cross_set |= kwg_woadwage_word_cross_set(kwg, before + letter_idx + 1,
                                               before_length - letter_idx - 1,
                                               after, after_length);
    }
  }
  // or in the first of two, before a second word that ends the letters after
  // it, all of which one walk back from the end finds.
  uint32_t node_index = kwg_get_root_node_index(kwg);
  for (int letter_idx = after_length - 1; letter_idx >= 0; letter_idx--) {
    if (!kwg_woadwage_follow(kwg, node_index, after[letter_idx], &node_index,
                             &accepts)) {
      break;
    }
    if (accepts) {
      cross_set |= kwg_woadwage_word_cross_set(kwg, before, before_length,
                                               after, letter_idx);
    }
  }
  return cross_set;
}
