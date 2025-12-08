#ifndef TRIE_H
#define TRIE_H

#include "../util/io_util.h"
#include "../util/string_util.h"

enum {
  TRIE_NUMBER_OF_CHILDREN = 256,
};

typedef struct Trie {
  int count;
  struct Trie *children[TRIE_NUMBER_OF_CHILDREN];
} Trie;

static inline Trie *trie_create(void) {
  return (Trie *)calloc_or_die(1, sizeof(Trie));
}

static inline void trie_destroy(Trie *trie) {
  if (!trie) {
    return;
  }
  for (int i = 0; i < TRIE_NUMBER_OF_CHILDREN; i++) {
    trie_destroy(trie->children[i]);
  }
  free(trie);
}

// Assumes the word only contains ASCII characters.
static inline void trie_add_word(Trie *trie, const char *word) {
  trie->count++;
  const size_t word_length = string_length(word);
  for (size_t i = 0; i < word_length; i++) {
    const int letter = word[i];
    if (!trie->children[letter]) {
      trie->children[letter] = trie_create();
    }
    trie = trie->children[letter];
    trie->count++;
  }
}

static inline int trie_get_shortest_unambiguous_index(const Trie *trie,
                                                      const char *word) {
  if (trie->count < 2) {
    return 0;
  }
  const size_t word_length = string_length(word);
  size_t i = 0;
  for (; i < word_length; i++) {
    const int letter = word[i];
    if (!trie->children[letter] || trie->children[letter]->count < 2) {
      break;
    }
    trie = trie->children[letter];
  }
  return (int)i + 1;
}

#endif
