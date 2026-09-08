#include "word_plus_floater_maker.h"

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/word_info_table.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "kwg_maker.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Validate the terminal layout before using it for construction. The source
// graph fingerprint alone cannot establish that a supplied WIT is complete.
static bool validate_wpf_trie_nodes(const WitTrie *trie, uint32_t first_node,
                                    int depth, int length, uint8_t *seen_nodes,
                                    uint8_t *seen_values) {
  MachineLetter previous_tile = 0;
  for (uint32_t node = first_node; node != 0; node++) {
    if (node >= trie->num_nodes || seen_nodes[node] || depth > length) {
      return false;
    }
    seen_nodes[node] = 1;
    const MachineLetter tile = trie->node_tile[node];
    if (tile <= previous_tile || tile > WPF_ALPHABET_SIZE ||
        trie->node_last[node] > 1) {
      return false;
    }
    previous_tile = tile;
    const int32_t value = trie->node_value[node];
    if (value < -1 || (value >= 0 && (depth != length ||
                                      (uint32_t)value >= trie->num_values ||
                                      seen_values[value]))) {
      return false;
    }
    if (value >= 0) {
      seen_values[value] = 1;
    }
    const uint32_t child = trie->node_child[node];
    if (child != 0 && !validate_wpf_trie_nodes(trie, child, depth + 1, length,
                                               seen_nodes, seen_values)) {
      return false;
    }
    if (trie->node_last[node]) {
      return true;
    }
  }
  return true;
}

static bool validate_wpf_trie(const WitTrie *trie, int length) {
  if (trie->num_nodes == 0 || trie->root >= trie->num_nodes ||
      trie->num_values > INT32_MAX || trie->node_tile == NULL ||
      trie->node_last == NULL || trie->node_child == NULL ||
      trie->node_value == NULL) {
    return false;
  }
  uint8_t *seen_nodes = calloc_or_die(trie->num_nodes, 1);
  uint8_t *seen_values =
      trie->num_values != 0 ? calloc_or_die(trie->num_values, 1) : NULL;
  bool valid = validate_wpf_trie_nodes(trie, trie->root, 1, length, seen_nodes,
                                       seen_values);
  for (uint32_t value = 0; valid && value < trie->num_values; value++) {
    valid = seen_values[value] != 0;
  }
  free(seen_values);
  free(seen_nodes);
  return valid;
}

// This helper is used only after the complete trie has passed validation.
static int32_t wpf_find_value(const WitTrie *trie, const MachineLetter *word,
                              int length) {
  uint32_t node = trie->root;
  for (int position = 0; position < length; position++) {
    if (node == 0) {
      return -1;
    }
    while (trie->node_tile[node] != word[position]) {
      if (trie->node_last[node]) {
        return -1;
      }
      node++;
    }
    if (position == length - 1) {
      return trie->node_value[node];
    }
    node = trie->node_child[node];
  }
  return -1;
}

static bool wpf_words_match_wit(const DictionaryWordList *words,
                                const WordInfoTable *wit) {
  uint8_t *seen_values[BOARD_DIM + 1] = {0};
  bool valid = true;
  for (int length = 1; length <= BOARD_DIM; length++) {
    const WitTrie *trie = &wit->tries[length];
    if (!validate_wpf_trie(trie, length)) {
      valid = false;
      break;
    }
    seen_values[length] =
        trie->num_values != 0 ? calloc_or_die(trie->num_values, 1) : NULL;
  }
  const int num_words = dictionary_word_list_get_count(words);
  for (int word_idx = 0; valid && word_idx < num_words; word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    const int length = dictionary_word_get_length(word);
    const MachineLetter *letters = dictionary_word_get_word(word);
    if (length < 1 || length > BOARD_DIM) {
      valid = false;
      break;
    }
    for (int position = 0; position < length; position++) {
      if (letters[position] == 0 || letters[position] > WPF_ALPHABET_SIZE) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      break;
    }
    const int32_t value = wpf_find_value(&wit->tries[length], letters, length);
    if (value < 0 || seen_values[length][value]) {
      valid = false;
      break;
    }
    seen_values[length][value] = 1;
  }
  for (int length = 1; length <= BOARD_DIM; length++) {
    for (uint32_t value = 0; valid && value < wit->tries[length].num_values;
         value++) {
      valid = seen_values[length][value] != 0;
    }
    free(seen_values[length]);
  }
  return valid;
}

static bool wpf_allocate_rows(WordInfoTable *wit) {
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    const uint32_t num_rows = wit->tries[length].num_values;
    const size_t stride = word_plus_floater_cells_per_key(length);
    if (num_rows > SIZE_MAX / stride ||
        (size_t)num_rows * stride > UINT32_MAX / sizeof(uint32_t)) {
      return false;
    }
    wit->word_plus_floater[length] =
        num_rows != 0
            ? calloc_or_die((size_t)num_rows * stride, sizeof(uint32_t))
            : NULL;
  }
  return true;
}

static void wpf_add_word(WordInfoTable *wit, const DictionaryWord *word) {
  const int final_length = dictionary_word_get_length(word);
  const MachineLetter *letters = dictionary_word_get_word(word);
  uint8_t counts[WPF_ALPHABET_SIZE + 1] = {0};
  uint32_t prefix[BOARD_DIM + 1] = {0};
  uint32_t suffix[BOARD_DIM + 1] = {0};
  for (int position = 0; position < final_length; position++) {
    counts[letters[position]]++;
    prefix[position + 1] = prefix[position] | (1U << letters[position]);
  }
  for (int position = final_length; position-- > 0;) {
    suffix[position] = suffix[position + 1] | (1U << letters[position]);
  }
  for (int length = WPF_MIN_BLOCK_LENGTH;
       length <= WPF_MAX_BLOCK_LENGTH && length < final_length; length++) {
    const int extension = final_length - length;
    const size_t stride = word_plus_floater_cells_per_key(length);
    for (int start = 0; start + length <= final_length; start++) {
      const int32_t value =
          wpf_find_value(&wit->tries[length], letters + start, length);
      if (value < 0) {
        continue;
      }
      assert(wit->word_plus_floater[length] != NULL);
      uint32_t *row = wit->word_plus_floater[length] + ((size_t)value * stride);
      uint8_t residual_counts[WPF_ALPHABET_SIZE + 1];
      memcpy(residual_counts, counts, sizeof(residual_counts));
      for (int position = start; position < start + length; position++) {
        residual_counts[letters[position]]--;
      }
      const uint32_t residual = prefix[start] | suffix[start + length];
      for (int floater_idx = 0; floater_idx < extension; floater_idx++) {
        const int position =
            floater_idx < start ? floater_idx : floater_idx + length;
        const MachineLetter floater = letters[position];
        // Skip the fixed base while enumerating floaters. Removing its width
        // makes both signed-position intervals contiguous in the cell row.
        const int offset = extension + floater_idx - start;
        const size_t cell = ((size_t)((extension * (extension - 1)) + offset) *
                             WPF_ALPHABET_SIZE) +
                            floater - 1;
        // Remove only this fixed occurrence. Another equal letter outside
        // the base and floater must remain in the residual mask.
        const uint32_t mask = residual_counts[floater] == 1
                                  ? residual & ~(1U << floater)
                                  : residual;
        row[cell] |= mask;
      }
    }
  }
}

void make_word_plus_floater_from_kwg(const KWG *kwg, WordInfoTable *wit,
                                     ErrorStack *error_stack) {
  word_info_table_clear_word_plus_floater(wit);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  if (wit->kwg_hash == 0 || wit->kwg_hash != kwg_get_hash(kwg)) {
    error_stack_push(
        error_stack, ERROR_STATUS_WIT_KWG_MISMATCH,
        string_duplicate("WordPlusFloater requires a matching KWG and WIT"));
    return;
  }
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), words, NULL);
  if (!wpf_words_match_wit(words, wit)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
        string_duplicate("WordPlusFloater requires the complete matching WIT "
                         "terminal set with machine letters 1..26"));
  } else if (!wpf_allocate_rows(wit)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
        string_duplicate(
            "WordPlusFloater rows exceed the supported table size"));
  } else {
    const int num_words = dictionary_word_list_get_count(words);
    for (int word_idx = 0; word_idx < num_words; word_idx++) {
      wpf_add_word(wit, dictionary_word_list_get_word(words, word_idx));
    }
  }
  dictionary_word_list_destroy(words);
  if (!error_stack_is_empty(error_stack)) {
    word_info_table_clear_word_plus_floater(wit);
  }
}
