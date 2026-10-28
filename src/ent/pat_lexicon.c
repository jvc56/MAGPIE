#include "pat_lexicon.h"

#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/pat_defs.h"
#include "../util/io_util.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "pat.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Row index of a key in the run-keyed tables: keys of each length are
// laid out consecutively (all 1-letter keys, then 2-letter, ...), each
// block indexed base alphabet_size in word order; within a row, word
// length then end.
static int pat_run_through_index(const PATWeights *pat, int word_end,
                                 const MachineLetter *key, int key_len,
                                 int word_length) {
  const int n = pat->run_through_alphabet_size;
  int block_start = 0;
  int block = 1;
  for (int key_idx = 1; key_idx < key_len; key_idx++) {
    block *= n;
    block_start += block;
  }
  // block_start now holds n + n^2 + ... + n^(key_len-1); add the key's
  // base-n value.
  int within = 0;
  for (int key_idx = 0; key_idx < key_len; key_idx++) {
    within = within * n + key[key_idx];
  }
  return ((block_start + within) * PAT_MAX_THROUGH_LEN + word_length) * 2 +
         word_end;
}

static int pat_run_through_rows(int alphabet_size) {
  int rows = 0;
  int block = 1;
  for (int key_len = 1; key_len <= PAT_RUN_THROUGH_MAX_KEY; key_len++) {
    block *= alphabet_size;
    rows += block;
  }
  return rows;
}

// The cell of key in the run-keyed tables, or -1 when it has none. In
// anagram mode the tables are keyed by the key's letters in ascending order
// (see pat_credit_multiset).
static int pat_run_through_cell(const PATWeights *pat, int word_end,
                                const MachineLetter *key, int key_len,
                                int word_length) {
  if (key_len < 1 || key_len > PAT_RUN_THROUGH_MAX_KEY ||
      word_length >= PAT_MAX_THROUGH_LEN) {
    return -1;
  }
  if (!pat->anagram) {
    return pat_run_through_index(pat, word_end, key, key_len, word_length);
  }
  MachineLetter sorted[PAT_RUN_THROUGH_MAX_KEY];
  for (int key_idx = 0; key_idx < key_len; key_idx++) {
    const MachineLetter ml = key[key_idx];
    int insert_idx = key_idx;
    while (insert_idx > 0 && sorted[insert_idx - 1] > ml) {
      sorted[insert_idx] = sorted[insert_idx - 1];
      insert_idx--;
    }
    sorted[insert_idx] = ml;
  }
  return pat_run_through_index(pat, word_end, sorted, key_len, word_length);
}

int pat_get_run_through_count(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length) {
  if (pat->run_through_count == NULL) {
    return 0;
  }
  const int cell =
      pat_run_through_cell(pat, word_end, key, key_len, word_length);
  return cell < 0 ? 0 : pat->run_through_count[cell];
}

int pat_get_run_through_score(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length) {
  if (pat->run_through_score == NULL) {
    return 0;
  }
  const int cell =
      pat_run_through_cell(pat, word_end, key, key_len, word_length);
  return cell < 0 ? 0 : pat->run_through_score[cell];
}

// Accumulators for the through table: for each (end letter, word length),
// the number of words and the summed tile value of everything in them but
// that end letter.
typedef struct PATThroughStats {
  // [0]: the letter is the word's first; [1]: its last.
  double count[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  double score_sum[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  // The run-keyed accumulators, indexed like the tables they become
  // (pat_run_through_index): the count of words with the key at that end
  // and the summed score of the rest of each such word.
  double *run_count;
  double *run_score_sum;
  const PATWeights *pat;
} PATThroughStats;

// Walks every word in the lexicon, crediting each to the letters at its two
// ends. A floater reaches the triple by being one end of the word that
// covers the span between them, so those are the only positions that
// matter; the value carried is what the REST of the word scores, which is
// what the opponent lays down to get there.
static void pat_walk_words(const KWG *kwg, const LetterDistribution *ld,
                           uint32_t node_index, MachineLetter *word, int length,
                           PATThroughStats *stats) {
  if (node_index == 0) {
    return;
  }
  for (uint32_t index = node_index;; index++) {
    const uint32_t node = kwg_node(kwg, index);
    const MachineLetter machine_letter = (MachineLetter)kwg_node_tile(node);
    word[length] = machine_letter;
    const int word_length = length + 1;
    if (kwg_node_accepts(node) && word_length >= MINIMUM_WORD_LENGTH &&
        word_length < PAT_MAX_THROUGH_LEN) {
      int total_score = 0;
      for (int letter_index = 0; letter_index < word_length; letter_index++) {
        total_score += equity_to_int(ld_get_score(ld, word[letter_index]));
      }
      const MachineLetter first = word[0];
      const MachineLetter last = word[word_length - 1];
      // Every prefix and suffix of the word up to the key depth, with what
      // the rest of the word scores.
      for (int key_len = 1;
           key_len <= PAT_RUN_THROUGH_MAX_KEY && key_len <= word_length;
           key_len++) {
        int prefix_score = 0;
        int suffix_score = 0;
        for (int key_idx = 0; key_idx < key_len; key_idx++) {
          prefix_score += equity_to_int(ld_get_score(ld, word[key_idx]));
          suffix_score += equity_to_int(
              ld_get_score(ld, word[word_length - key_len + key_idx]));
        }
        const int prefix_index =
            pat_run_through_index(stats->pat, 0, word, key_len, word_length);
        const int suffix_index = pat_run_through_index(
            stats->pat, 1, word + word_length - key_len, key_len, word_length);
        stats->run_count[prefix_index] += 1.0;
        stats->run_score_sum[prefix_index] += total_score - prefix_score;
        stats->run_count[suffix_index] += 1.0;
        stats->run_score_sum[suffix_index] += total_score - suffix_score;
      }
      stats->count[0][first][word_length] += 1.0;
      stats->score_sum[0][first][word_length] +=
          total_score - equity_to_int(ld_get_score(ld, first));
      // The last letter is a distinct position even when it repeats the
      // first, since accepted words are at least two letters long.
      stats->count[1][last][word_length] += 1.0;
      stats->score_sum[1][last][word_length] +=
          total_score - equity_to_int(ld_get_score(ld, last));
    }
    if (word_length < PAT_MAX_THROUGH_LEN - 1) {
      pat_walk_words(kwg, ld, kwg_node_arc_index(node), word, word_length,
                     stats);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

// A word's letters in ascending order, zero past its length so that equal
// multisets compare equal byte for byte.
typedef struct PATMultiset {
  uint8_t length;
  MachineLetter letters[PAT_MAX_THROUGH_LEN - 1];
} PATMultiset;

typedef struct PATMultisetList {
  PATMultiset *items;
  size_t count;
  size_t capacity;
} PATMultisetList;

// Collects the letters of every word in the lexicon shorter than
// PAT_MAX_THROUGH_LEN, sorted, walking it as pat_walk_words does.
static void pat_collect_multisets(const KWG *kwg, uint32_t node_index,
                                  MachineLetter *word, int length,
                                  PATMultisetList *list) {
  if (node_index == 0) {
    return;
  }
  for (uint32_t index = node_index;; index++) {
    const uint32_t node = kwg_node(kwg, index);
    word[length] = (MachineLetter)kwg_node_tile(node);
    const int word_length = length + 1;
    if (kwg_node_accepts(node) && word_length >= MINIMUM_WORD_LENGTH &&
        word_length < PAT_MAX_THROUGH_LEN) {
      if (list->count == list->capacity) {
        list->capacity = list->capacity == 0 ? 4096 : list->capacity * 2;
        list->items =
            realloc_or_die(list->items, list->capacity * sizeof(PATMultiset));
      }
      PATMultiset *multiset = &list->items[list->count++];
      memset(multiset, 0, sizeof(*multiset));
      multiset->length = (uint8_t)word_length;
      for (int letter_idx = 0; letter_idx < word_length; letter_idx++) {
        const MachineLetter ml = word[letter_idx];
        int insert_idx = letter_idx;
        while (insert_idx > 0 && multiset->letters[insert_idx - 1] > ml) {
          multiset->letters[insert_idx] = multiset->letters[insert_idx - 1];
          insert_idx--;
        }
        multiset->letters[insert_idx] = ml;
      }
    }
    if (word_length < PAT_MAX_THROUGH_LEN - 1) {
      pat_collect_multisets(kwg, kwg_node_arc_index(node), word, word_length,
                            list);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

static int pat_compare_multisets(const void *a, const void *b) {
  return memcmp(a, b, sizeof(PATMultiset));
}

// Credits the run-keyed accumulators with each sub-multiset of the word's
// letters of up to PAT_RUN_THROUGH_MAX_KEY letters, in ascending order: the
// distinct letters from first_idx on, each taken at most as often as the word
// holds it.
static void pat_credit_anagram_keys(const MachineLetter *distinct,
                                    const int *multiplicities, int num_distinct,
                                    int first_idx, MachineLetter *key,
                                    int key_len, int key_score, int total_score,
                                    int word_length,
                                    const LetterDistribution *ld,
                                    PATThroughStats *stats) {
  if (key_len > 0) {
    for (int word_end = 0; word_end <= 1; word_end++) {
      const int index = pat_run_through_index(stats->pat, word_end, key,
                                              key_len, word_length);
      stats->run_count[index] += 1.0;
      stats->run_score_sum[index] += total_score - key_score;
    }
  }
  if (key_len == PAT_RUN_THROUGH_MAX_KEY) {
    return;
  }
  for (int distinct_idx = first_idx; distinct_idx < num_distinct;
       distinct_idx++) {
    int used = 0;
    for (int key_idx = 0; key_idx < key_len; key_idx++) {
      used += key[key_idx] == distinct[distinct_idx];
    }
    if (used == multiplicities[distinct_idx]) {
      continue;
    }
    key[key_len] = distinct[distinct_idx];
    pat_credit_anagram_keys(
        distinct, multiplicities, num_distinct, distinct_idx, key, key_len + 1,
        key_score + equity_to_int(ld_get_score(ld, distinct[distinct_idx])),
        total_score, word_length, ld, stats);
  }
}

// Credits one word multiset in anagram mode. Any arrangement of it is a word,
// so each of its letters can sit at either end, and a run's letters at
// either end in any order: the through tables credit every distinct letter
// at both ends, and the run tables every sub-multiset, keyed in ascending
// order. A two-letter multiset makes a word in both orders.
static void pat_credit_multiset(const PATMultiset *multiset,
                                const LetterDistribution *ld,
                                PATThroughStats *stats,
                                int counts[MAX_ALPHABET_SIZE]) {
  const int word_length = multiset->length;
  int total_score = 0;
  MachineLetter distinct[PAT_MAX_THROUGH_LEN] = {0};
  int multiplicities[PAT_MAX_THROUGH_LEN] = {0};
  int num_distinct = 0;
  for (int letter_idx = 0; letter_idx < word_length; letter_idx++) {
    const MachineLetter ml = multiset->letters[letter_idx];
    total_score += equity_to_int(ld_get_score(ld, ml));
    if (num_distinct > 0 && distinct[num_distinct - 1] == ml) {
      multiplicities[num_distinct - 1]++;
    } else {
      distinct[num_distinct] = ml;
      multiplicities[num_distinct] = 1;
      num_distinct++;
    }
  }
  if (word_length == 2) {
    // As pat_count_two_letter_words counts each word's two letters: both
    // orders when they differ, the one word when they are the same.
    for (int letter_idx = 0; letter_idx < 2; letter_idx++) {
      counts[multiset->letters[letter_idx]] += num_distinct;
    }
  }
  for (int distinct_idx = 0; distinct_idx < num_distinct; distinct_idx++) {
    const MachineLetter ml = distinct[distinct_idx];
    for (int word_end = 0; word_end <= 1; word_end++) {
      stats->count[word_end][ml][word_length] += 1.0;
      stats->score_sum[word_end][ml][word_length] +=
          total_score - equity_to_int(ld_get_score(ld, ml));
    }
  }
  MachineLetter key[PAT_RUN_THROUGH_MAX_KEY];
  pat_credit_anagram_keys(distinct, multiplicities, num_distinct, 0, key, 0, 0,
                          total_score, word_length, ld, stats);
}

// The anagram mode's walk: the lexicon's distinct word multisets, each
// credited once however many words spell it.
static void pat_walk_multisets(const KWG *kwg, const LetterDistribution *ld,
                               PATThroughStats *stats,
                               int counts[MAX_ALPHABET_SIZE]) {
  PATMultisetList list = {.items = NULL, .count = 0, .capacity = 0};
  MachineLetter word[PAT_MAX_THROUGH_LEN];
  pat_collect_multisets(kwg, kwg_get_dawg_root_node_index(kwg), word, 0, &list);
  qsort(list.items, list.count, sizeof(PATMultiset), pat_compare_multisets);
  for (size_t item_idx = 0; item_idx < list.count; item_idx++) {
    if (item_idx > 0 && pat_compare_multisets(&list.items[item_idx - 1],
                                              &list.items[item_idx]) == 0) {
      continue;
    }
    pat_credit_multiset(&list.items[item_idx], ld, stats, counts);
  }
  free(list.items);
}

// Counts, for each letter, the two-letter words containing it.
static void pat_count_two_letter_words(const KWG *kwg,
                                       const LetterDistribution *ld,
                                       int counts[MAX_ALPHABET_SIZE]) {
  const uint32_t dawg_root = kwg_get_dawg_root_node_index(kwg);
  const int ld_size = ld_get_size(ld);
  for (int first_ml = 1; first_ml < ld_size; first_ml++) {
    const uint32_t node_index =
        kwg_get_next_node_index(kwg, dawg_root, (MachineLetter)first_ml);
    if (node_index == 0) {
      continue;
    }
    uint64_t extension_set = 0;
    const uint64_t accepted_set =
        kwg_get_letter_sets(kwg, node_index, &extension_set) & ~(uint64_t)1;
    for (int second_ml = 1; second_ml < ld_size; second_ml++) {
      if (accepted_set & ((uint64_t)1 << second_ml)) {
        counts[first_ml]++;
        counts[second_ml]++;
      }
    }
  }
}

void pat_prepare_hook_flex(PATWeights *pat, const KWG *kwg,
                           const LetterDistribution *ld, bool anagram) {
  const uint64_t kwg_hash = kwg != NULL ? kwg_get_hash(kwg) : 0;
  if (kwg != NULL && pat->prepared && pat->anagram == anagram &&
      pat->prepared_kwg_hash == kwg_hash && pat->prepared_ld == ld) {
    return;
  }
  pat->prepared_kwg_hash = kwg_hash;
  pat->prepared_ld = ld;
  pat->prepared = true;
  pat->anagram = anagram;
  memset(pat->hook_flex, 0, sizeof(pat->hook_flex));
  memset(pat->through_score, 0, sizeof(pat->through_score));
  memset(pat->through_count, 0, sizeof(pat->through_count));
  memset(pat->through_score_end, 0, sizeof(pat->through_score_end));
  memset(pat->through_count_end, 0, sizeof(pat->through_count_end));
  if (!kwg) {
    return;
  }
  const int ld_size = ld_get_size(ld);
  PATThroughStats *stats = calloc_or_die(1, sizeof(PATThroughStats));
  pat->run_through_alphabet_size = ld_size;
  const size_t run_cells =
      (size_t)pat_run_through_rows(ld_size) * PAT_MAX_THROUGH_LEN * 2;
  stats->run_count = calloc_or_die(run_cells, sizeof(double));
  stats->run_score_sum = calloc_or_die(run_cells, sizeof(double));
  stats->pat = pat;
  int counts[MAX_ALPHABET_SIZE] = {0};
  if (anagram) {
    pat_walk_multisets(kwg, ld, stats, counts);
  } else {
    pat_count_two_letter_words(kwg, ld, counts);
    MachineLetter word[PAT_MAX_THROUGH_LEN];
    pat_walk_words(kwg, ld, kwg_get_dawg_root_node_index(kwg), word, 0, stats);
  }
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    if (counts[ml] > UINT8_MAX) {
      counts[ml] = UINT8_MAX;
    }
    pat->hook_flex[ml] = (uint8_t)counts[ml];
  }
  free(pat->run_through_count);
  free(pat->run_through_score);
  pat->run_through_count = calloc_or_die(run_cells, sizeof(uint8_t));
  pat->run_through_score = calloc_or_die(run_cells, sizeof(uint8_t));
  for (size_t cell = 0; cell < run_cells; cell++) {
    const double word_count = stats->run_count[cell];
    if (word_count <= 0.0) {
      continue;
    }
    double mean_score = stats->run_score_sum[cell] / word_count;
    if (mean_score > UINT8_MAX) {
      mean_score = UINT8_MAX;
    }
    double scaled = 8.0 * log2(1.0 + word_count);
    if (scaled > UINT8_MAX) {
      scaled = UINT8_MAX;
    }
    pat->run_through_score[cell] = (uint8_t)(mean_score + 0.5);
    pat->run_through_count[cell] = (uint8_t)(scaled + 0.5);
  }
  free(stats->run_count);
  free(stats->run_score_sum);
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    for (int len = 0; len < PAT_MAX_THROUGH_LEN; len++) {
      // The unsigned tables are exactly what they always were: both ends
      // pooled into one count and one mean.
      for (int word_end = 0; word_end <= 2; word_end++) {
        const double word_count =
            (word_end < 2)
                ? stats->count[word_end][ml][len]
                : stats->count[0][ml][len] + stats->count[1][ml][len];
        if (word_count <= 0.0) {
          continue;
        }
        const double score_sum =
            (word_end < 2)
                ? stats->score_sum[word_end][ml][len]
                : stats->score_sum[0][ml][len] + stats->score_sum[1][ml][len];
        double mean_score = score_sum / word_count;
        if (mean_score > UINT8_MAX) {
          mean_score = UINT8_MAX;
        }
        // Log scale: the useful distinction is between a letter that
        // reaches nothing, a few words, and thousands, not between 900
        // and 1000.
        double scaled = 8.0 * log2(1.0 + word_count);
        if (scaled > UINT8_MAX) {
          scaled = UINT8_MAX;
        }
        if (word_end < 2) {
          pat->through_score_end[word_end][ml][len] =
              (uint8_t)(mean_score + 0.5);
          pat->through_count_end[word_end][ml][len] = (uint8_t)(scaled + 0.5);
        } else {
          pat->through_score[ml][len] = (uint8_t)(mean_score + 0.5);
          pat->through_count[ml][len] = (uint8_t)(scaled + 0.5);
        }
      }
    }
  }
  free(stats);
}

int pat_get_through_score_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span) {
  return (span < PAT_MAX_THROUGH_LEN)
             ? pat->through_score_end[word_end][ml][span]
             : 0;
}

int pat_get_through_count_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span) {
  return (span < PAT_MAX_THROUGH_LEN)
             ? pat->through_count_end[word_end][ml][span]
             : 0;
}

int pat_get_through_count(const PATWeights *pat, MachineLetter ml, int span) {
  return (span < PAT_MAX_THROUGH_LEN) ? pat->through_count[ml][span] : 0;
}

int pat_get_hook_flex(const PATWeights *pat, MachineLetter ml) {
  return pat->hook_flex[ml];
}
