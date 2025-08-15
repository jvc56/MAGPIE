#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../util/io_util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The KWG data structure was originally
// developed in wolges. For more details
// on how the KWG data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt

// This has a subset of the logic for KWG creation, and only merges arcs
// for nodes sharing all of their subtries, whereas
// the reference implementation in wolges merges arcs can merge nodes where
// one's subtries are a tail of the other's, with the node with fewer children
// pointing midway into the the other's list of children, sharing an end but not
// a beginning. This extra merging reduces KWG size roughly 3%.

typedef struct NodeIndexList {
  union {
    uint32_t inline_indices[KWG_NODE_INDEX_LIST_INLINE_CAPACITY];
    struct {
      uint32_t *indices;
      uint32_t padding_indices[KWG_NODE_INDEX_LIST_INLINE_CAPACITY - 2];
    };
  };
  size_t count;
  size_t capacity;
} NodeIndexList;

static inline bool node_index_list_is_inline(const NodeIndexList *list) {
  return list->capacity <= KWG_NODE_INDEX_LIST_INLINE_CAPACITY;
}

static inline uint32_t *node_index_list_get_indices(NodeIndexList *list) {
  if (node_index_list_is_inline(list)) {
    return list->inline_indices;
  }
  return list->indices;
}

static inline const uint32_t *
node_index_list_get_const_indices(const NodeIndexList *list) {
  if (node_index_list_is_inline(list)) {
    return list->inline_indices;
  }
  return list->indices;
}

static inline void node_index_list_initialize(NodeIndexList *list) {
  list->capacity = KWG_NODE_INDEX_LIST_INLINE_CAPACITY;
  list->count = 0;
}

static inline void node_index_list_add(NodeIndexList *list, uint32_t index) {
  if (list->count == list->capacity) {
    if (node_index_list_is_inline(list)) {
      list->capacity *= 2;
      uint32_t *indices = malloc_or_die(sizeof(uint32_t) * 2 *
                                        KWG_NODE_INDEX_LIST_INLINE_CAPACITY);
      memcpy(indices, list->inline_indices,
             sizeof(uint32_t) * KWG_NODE_INDEX_LIST_INLINE_CAPACITY);
      list->indices = indices;
    } else {
      list->capacity *= 2;
      list->indices =
          realloc_or_die(list->indices, sizeof(uint32_t) * list->capacity);
    }
  }
  uint32_t *indices = node_index_list_get_indices(list);
  indices[list->count] = index;
  list->count++;
}

static inline void node_index_list_destroy(NodeIndexList *list) {
  if (!node_index_list_is_inline(list)) {
    free(list->indices);
  }
}

typedef struct MutableNode {
  MachineLetter ml;
  bool accepts;
  bool is_end;
  NodeIndexList children;
  uint64_t hash_with_just_children;
  uint64_t hash_with_node;
  struct MutableNode *merged_into;
  uint8_t merge_offset;
  uint32_t final_index;
} MutableNode;

typedef struct MutableNodeList {
  MutableNode *nodes;
  size_t count;
  size_t capacity;
} MutableNodeList;

static inline MutableNodeList *mutable_node_list_create(void) {
  MutableNodeList *mutable_node_list = malloc_or_die(sizeof(MutableNodeList));
  mutable_node_list->capacity = KWG_MUTABLE_NODE_LIST_INITIAL_CAPACITY;
  mutable_node_list->nodes =
      malloc_or_die(sizeof(MutableNode) * mutable_node_list->capacity);
  mutable_node_list->count = 0;
  return mutable_node_list;
}

static inline MutableNode *mutable_node_list_add(MutableNodeList *nodes) {
  if (nodes->count == nodes->capacity) {
    nodes->nodes =
        realloc_or_die(nodes->nodes, sizeof(MutableNode) * nodes->capacity * 2);
    nodes->capacity *= 2;
  }
  MutableNode *node = &nodes->nodes[nodes->count];
  node->ml = 0;
  node->accepts = false;
  node->is_end = false;
  node->merged_into = NULL;
  node->merge_offset = 0;
  nodes->count++;
  return node;
}

static inline int mutable_node_list_add_root(MutableNodeList *nodes) {
  const size_t root_node_index = nodes->count;
  MutableNode *root = mutable_node_list_add(nodes);
  node_index_list_initialize(&root->children);
  return (int)root_node_index;
}

static inline int add_child(uint32_t node_index, MutableNodeList *nodes,
                            MachineLetter ml) {
  const size_t child_node_index = nodes->count;
  MutableNode *node = &nodes->nodes[node_index];
  node_index_list_add(&node->children, child_node_index);
  MutableNode *child = mutable_node_list_add(nodes);
  child->ml = ml;
  node_index_list_initialize(&child->children);
  return (int)child_node_index;
}

static inline void mutable_node_list_destroy(MutableNodeList *nodes) {
  for (size_t i = 0; i < nodes->count; i++) {
    node_index_list_destroy(&nodes->nodes[i].children);
  }
  free(nodes->nodes);
  free(nodes);
}

uint64_t subtree_hash_value(MutableNode *node) {
  uint64_t hash_with_node = node->hash_with_just_children;
  const MachineLetter ml = node->ml;
  const bool accepts = node->accepts;
  hash_with_node ^= 1 + ml;
  if (accepts) {
    // Most Scrabble languages including English have <32 letters and fit in 5
    // bits so this hash function is optimized for them. Polish has 33 including
    // the blank and so this is not ideal for it, but it is still valid, and we
    // can revisit this to work better for large dictionaries if we choose to.
    hash_with_node ^= 1 << (ENGLISH_ALPHABET_BITS_USED + 1);
  }
  node->hash_with_node = hash_with_node;
  return hash_with_node;
}

static inline uint64_t mutable_node_hash_value(MutableNode *node,
                                               MutableNode *nodes) {
  uint64_t hash_with_just_children = 0;

  const size_t children_count = node->children.count;
  const uint32_t *indices = node_index_list_get_const_indices(&node->children);
  for (size_t i = 0; i < children_count; i++) {
    const size_t child_index = indices[i];
    if (child_index != 0) {
      MutableNode *child = &nodes[child_index];
      uint64_t child_hash = subtree_hash_value(child);
      hash_with_just_children ^= child_hash * KWG_HASH_COMBINING_PRIME;
    }
  }
  // rotate by one bit to designate the end of the child list
  hash_with_just_children =
      (hash_with_just_children << 1) | (hash_with_just_children >> (64 - 1));

  node->hash_with_just_children = hash_with_just_children;
  return hash_with_just_children;
}

void calculate_node_hash_values(MutableNodeList *node_list) {
  // Traverse the nodes in reverse order (bottom-up)
  const size_t count = node_list->count;
  MutableNode *nodes = node_list->nodes;
  if (count == 0) {
    return;
  }
  for (size_t i = count; i > 0; i--) {
    MutableNode *node = &nodes[i - 1];
    mutable_node_hash_value(node, nodes);
  }
}

bool subtree_equals(const MutableNode *node_a, const MutableNode *node_b,
                    const MutableNode *nodes) {
  // Hashes were already compared, don't check them again.
  if (node_a->ml != node_b->ml || node_a->accepts != node_b->accepts) {
    return false;
  }
  if (node_a->children.count != node_b->children.count) {
    return false;
  }
  const size_t count = node_a->children.count;
  const uint32_t *indices_a =
      node_index_list_get_const_indices(&node_a->children);
  const uint32_t *indices_b =
      node_index_list_get_const_indices(&node_b->children);
  for (size_t idx = 0; idx < count; idx++) {
    const MutableNode *child_a = &nodes[indices_a[idx]];
    const MutableNode *child_b = &nodes[indices_b[idx]];
    if (child_a->hash_with_node != child_b->hash_with_node) {
      return false;
    }
    if (!subtree_equals(child_a, child_b, nodes)) {
      return false;
    }
  }
  return true;
}

bool mutable_node_equals(const MutableNode *node_a, const MutableNode *node_b,
                         const MutableNode *nodes) {
  // Ignores node_x->ml and node_x->accepts
  if (node_a->hash_with_just_children != node_b->hash_with_just_children) {
    return false;
  }
  if (node_a->children.count != node_b->children.count) {
    return false;
  }
  const size_t count = node_a->children.count;
  const uint32_t *indices_a =
      node_index_list_get_const_indices(&node_a->children);
  const uint32_t *indices_b =
      node_index_list_get_const_indices(&node_b->children);
  for (size_t idx = 0; idx < count; idx++) {
    const MutableNode *child_a = &nodes[indices_a[idx]];
    const MutableNode *child_b = &nodes[indices_b[idx]];
    if (child_a->hash_with_node != child_b->hash_with_node) {
      return false;
    }
    if (!subtree_equals(child_a, child_b, nodes)) {
      return false;
    }
  }
  return true;
}

typedef struct NodePointerList {
  MutableNode **nodes;
  size_t count;
  size_t capacity;
} NodePointerList;

static inline NodePointerList *node_pointer_list_create(void) {
  NodePointerList *node_pointer_list = malloc_or_die(sizeof(NodePointerList));
  node_pointer_list->capacity = KWG_ORDERED_POINTER_LIST_INITIAL_CAPACITY;
  node_pointer_list->nodes = malloc_or_die(
      sizeof(MutableNode *) * KWG_ORDERED_POINTER_LIST_INITIAL_CAPACITY);
  node_pointer_list->count = 0;
  return node_pointer_list;
}

static inline void node_pointer_list_add(NodePointerList *list,
                                         MutableNode *node) {
  if (list->count == list->capacity) {
    list->nodes =
        realloc_or_die(list->nodes, sizeof(MutableNode *) * list->capacity * 2);
    list->capacity *= 2;
  }
  list->nodes[list->count] = node;
  list->count++;
}

static inline void node_pointer_list_destroy(NodePointerList *list) {
  free(list->nodes);
  free(list);
}

typedef struct NodeHashTable {
  uint32_t *bucket_heads;
  uint32_t *next_indices;
  size_t node_capacity;
} NodeHashTable;

static inline void node_hash_table_create(NodeHashTable *table,
                                          size_t node_capacity) {
  table->node_capacity = node_capacity;
  table->bucket_heads =
      malloc_or_die(sizeof(uint32_t) * KWG_HASH_NUMBER_OF_BUCKETS);
  table->next_indices = malloc_or_die(sizeof(uint32_t) * table->node_capacity);
  for (size_t i = 0; i < KWG_HASH_NUMBER_OF_BUCKETS; i++) {
    table->bucket_heads[i] = HASH_BUCKET_ITEM_LIST_NULL_INDEX;
  }
  for (size_t i = 0; i < table->node_capacity; i++) {
    table->next_indices[i] = HASH_BUCKET_ITEM_LIST_NULL_INDEX;
  }
}

static inline void node_hash_table_destroy_buckets(NodeHashTable *table) {
  free(table->bucket_heads);
  free(table->next_indices);
}

static inline MutableNode *node_hash_table_find_or_insert(NodeHashTable *table,
                                                          MutableNode *node,
                                                          MutableNode *nodes) {
  const uint32_t node_index = node - nodes;
  const uint64_t hash_value = node->hash_with_just_children;
  const size_t bucket_index = hash_value % KWG_HASH_NUMBER_OF_BUCKETS;

  uint32_t current_index = table->bucket_heads[bucket_index];
  uint32_t previous_index = HASH_BUCKET_ITEM_LIST_NULL_INDEX;

  while (current_index != HASH_BUCKET_ITEM_LIST_NULL_INDEX) {
    MutableNode *candidate = &nodes[current_index];
    if (mutable_node_equals(node, candidate, nodes)) {
      return candidate;
    }
    previous_index = current_index;
    current_index = table->next_indices[current_index];
  }

  if (previous_index == HASH_BUCKET_ITEM_LIST_NULL_INDEX) {
    table->bucket_heads[bucket_index] = node_index;
  } else {
    table->next_indices[previous_index] = node_index;
  }
  table->next_indices[node_index] = HASH_BUCKET_ITEM_LIST_NULL_INDEX;
  return node;
}

uint32_t get_child_index(const MutableNode *node, size_t idx) {
  const uint32_t *indices =
      node_index_list_get_const_indices((NodeIndexList *)&node->children);
  return indices[idx];
}

void set_final_indices(MutableNode *node, MutableNodeList *nodes,
                       NodePointerList *ordered_pointers) {
  // Add the children in a sequence.
  for (size_t i = 0; i < node->children.count; i++) {
    const uint32_t child_index = get_child_index(node, i);
    MutableNode *child = &nodes->nodes[child_index];
    child->is_end = (i == (node->children.count - 1));
    child->final_index = ordered_pointers->count;
    node_pointer_list_add(ordered_pointers, child);
  }
  // Then add each of their subtries afterwards.
  for (size_t i = 0; i < node->children.count; i++) {
    const uint32_t child_index = get_child_index(node, i);
    MutableNode *child = &nodes->nodes[child_index];
    if (child->merged_into != NULL) {
      continue;
    }
    set_final_indices(child, nodes, ordered_pointers);
  }
}

void insert_suffix(uint32_t node_index, MutableNodeList *nodes,
                   const DictionaryWord *word, int pos,
                   int *cached_node_indices) {
  MutableNode *node = &nodes->nodes[node_index];
  const int length = dictionary_word_get_length(word);
  if (pos == length) {
    node->accepts = true;
    return;
  }
  const int ml = dictionary_word_get_word(word)[pos];
  const uint8_t node_num_children = node->children.count;
  for (MachineLetter i = 0; i < node_num_children; i++) {
    node = &nodes->nodes[node_index];
    const uint32_t child_index = get_child_index(node, i);
    const MutableNode *child = &nodes->nodes[child_index];
    if (child->ml == ml) {
      insert_suffix(child_index, nodes, word, pos + 1, cached_node_indices);
      return;
    }
  }
  const int child_index = add_child(node_index, nodes, ml);
  cached_node_indices[pos + 1] = child_index;
  insert_suffix(child_index, nodes, word, pos + 1, cached_node_indices);
}

void copy_nodes(NodePointerList *ordered_pointers, MutableNodeList *nodes,
                const KWG *kwg) {
  uint32_t *kwg_nodes = kwg_get_mutable_nodes(kwg);
  for (size_t node_idx = 0; node_idx < ordered_pointers->count; node_idx++) {
    MutableNode *node = ordered_pointers->nodes[node_idx];
    uint32_t serialized_node = node->ml << KWG_TILE_BIT_OFFSET;
    if (node->accepts) {
      serialized_node |= KWG_NODE_ACCEPTS_FLAG;
    }
    if (node->is_end) {
      serialized_node |= KWG_NODE_IS_END_FLAG;
    }
    if (node->children.count > 0) {
      NodeIndexList *children = (node->merged_into == NULL)
                                    ? &node->children
                                    : &node->merged_into->children;
      const uint32_t *indices = node_index_list_get_indices(children);
      const uint32_t original_child_index = indices[0];
      const uint32_t final_child_index =
          nodes->nodes[original_child_index].final_index;
      serialized_node |= final_child_index;
    }
    kwg_nodes[node_idx] = serialized_node;
  }
}

static inline void
add_gaddag_strings_for_word(const DictionaryWord *word,
                            DictionaryWordList *gaddag_strings) {
  const MachineLetter *raw_word = dictionary_word_get_word(word);
  const int length = dictionary_word_get_length(word);
  MachineLetter gaddag_string[MAX_KWG_STRING_LENGTH];
  // First add the word reversed without the separator.
  for (int i = 0; i < length; i++) {
    const int source_index = length - i - 1;
    gaddag_string[i] = raw_word[source_index];
  }
  // cppcheck-suppress uninitvar
  dictionary_word_list_add_word(gaddag_strings, gaddag_string, length);
  // Add the word with separator pivoting at each position from length-1 to 0.
  for (int separator_pos = length - 1; separator_pos >= 1; separator_pos--) {
    for (int i = 0; i < separator_pos; i++) {
      gaddag_string[i] = raw_word[separator_pos - i - 1];
    }
    gaddag_string[separator_pos] = SEPARATION_MACHINE_LETTER;
    // We want the rest of the word (forwards) after the separator.
    // Only one letter should need to be moved each time.
    gaddag_string[separator_pos + 1] = raw_word[separator_pos];
    dictionary_word_list_add_word(gaddag_strings, gaddag_string, length + 1);
  }
}

void add_gaddag_strings(const DictionaryWordList *words,
                        DictionaryWordList *gaddag_strings) {
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    add_gaddag_strings_for_word(word, gaddag_strings);
  }
  dictionary_word_list_sort(gaddag_strings);
}

void write_words_aux(const KWG *kwg, uint32_t node_index, MachineLetter *prefix,
                     int prefix_length, int max_length, bool accepts,
                     DictionaryWordList *words, bool *nodes_reached) {
  if (accepts && (prefix_length <= max_length)) {
    dictionary_word_list_add_word(words, prefix, prefix_length);
  }
  if (node_index == 0) {
    return;
  }
  for (uint32_t i = node_index;; i++) {
    if (nodes_reached != NULL) {
      nodes_reached[i] = true;
    }
    const uint32_t node = kwg_node(kwg, i);
    const MachineLetter ml = kwg_node_tile(node);
    const uint32_t new_node_index = kwg_node_arc_index_prefetch(node, kwg);
    const bool node_accepts = kwg_node_accepts(node);
    if (prefix_length < max_length) {
      prefix[prefix_length] = ml;
    }
    write_words_aux(kwg, new_node_index, prefix, prefix_length + 1, max_length,
                    node_accepts, words, nodes_reached);
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void kwg_write_words(const KWG *kwg, uint32_t node_index,
                     DictionaryWordList *words, bool *nodes_reached) {
  MachineLetter prefix[BOARD_DIM];
  write_words_aux(kwg, node_index, prefix, 0, BOARD_DIM, false, words,
                  nodes_reached);
}

void kwg_write_gaddag_strings(const KWG *kwg, uint32_t node_index,
                              DictionaryWordList *gaddag_strings,
                              bool *nodes_reached) {
  MachineLetter prefix[MAX_KWG_STRING_LENGTH];
  write_words_aux(kwg, node_index, prefix, 0, MAX_KWG_STRING_LENGTH, false,
                  gaddag_strings, nodes_reached);
}

static inline int get_letters_in_common(const DictionaryWord *word,
                                        MachineLetter *last_word,
                                        int *last_word_length) {
  const int length = dictionary_word_get_length(word);
  int min_length = length;
  if (*last_word_length < min_length) {
    min_length = *last_word_length;
  }
  int letters_in_common = 0;
  for (int k = 0; k < min_length; k++) {
    if (dictionary_word_get_word(word)[k] == last_word[k]) {
      letters_in_common++;
    } else {
      break;
    }
  }
  *last_word_length = length;
  memcpy(last_word, dictionary_word_get_word(word), length);
  return letters_in_common;
}

// The dictionary word list must be in alphabetical order.
KWG *make_kwg_from_words(const DictionaryWordList *words,
                         kwg_maker_output_t output, kwg_maker_merge_t merging) {
  const bool output_dawg = (output == KWG_MAKER_OUTPUT_DAWG) ||
                           (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);
  const bool output_gaddag = (output == KWG_MAKER_OUTPUT_GADDAG) ||
                             (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);
  MutableNodeList *nodes = mutable_node_list_create();
  const int dawg_root_node_index = mutable_node_list_add_root(nodes);
  // Size is one beyond the longest string because nodes are created for
  // potential children at the max+1'th, though there are none.
  int cached_node_indices[MAX_KWG_STRING_LENGTH + 1];
  MachineLetter last_word[MAX_KWG_STRING_LENGTH];
  int last_word_length = 0;
  for (size_t i = 0; i < MAX_KWG_STRING_LENGTH; i++) {
    last_word[i] = 0;
  }
  if (output_dawg) {
    cached_node_indices[0] = dawg_root_node_index;
    const int words_count = dictionary_word_list_get_count(words);
    for (int i = 0; i < words_count; i++) {
      const DictionaryWord *word = dictionary_word_list_get_word(words, i);
      const int letters_in_common =
          get_letters_in_common(word, last_word, &last_word_length);
      const int start_index = cached_node_indices[letters_in_common];
      insert_suffix(start_index, nodes, word, letters_in_common,
                    cached_node_indices);
    }
  }

  const int gaddag_root_node_index = mutable_node_list_add_root(nodes);
  if (output_gaddag) {
    last_word_length = 0;
    cached_node_indices[0] = gaddag_root_node_index;
    DictionaryWordList *gaddag_strings = dictionary_word_list_create();
    add_gaddag_strings(words, gaddag_strings);
    for (int i = 0; i < dictionary_word_list_get_count(gaddag_strings); i++) {
      const DictionaryWord *gaddag_string =
          dictionary_word_list_get_word(gaddag_strings, i);
      const int letters_in_common =
          get_letters_in_common(gaddag_string, last_word, &last_word_length);
      const int start_index = cached_node_indices[letters_in_common];
      insert_suffix(start_index, nodes, gaddag_string, letters_in_common,
                    cached_node_indices);
    }
    dictionary_word_list_destroy(gaddag_strings);
  }

  if (merging == KWG_MAKER_MERGE_EXACT) {
    calculate_node_hash_values(nodes);
    NodeHashTable table;
    node_hash_table_create(&table, nodes->count);
    const size_t count = nodes->count;
    MutableNode *nodes_array = nodes->nodes;
    for (size_t i = 0; i < count; i++) {
      if (!output_dawg && (i == 0)) {
        continue;
      }
      if (!output_gaddag && (i == 1)) {
        continue;
      }
      MutableNode *node = &nodes_array[i];
      MutableNode *match =
          node_hash_table_find_or_insert(&table, node, nodes_array);
      if (match == node) {
        continue;
      }
      node->merged_into = match;
    }
    node_hash_table_destroy_buckets(&table);
  }

  MutableNode *dawg_root = &nodes->nodes[dawg_root_node_index];
  dawg_root->is_end = true;
  MutableNode *gaddag_root = &nodes->nodes[gaddag_root_node_index];
  gaddag_root->is_end = true;
  NodePointerList *ordered_pointers = node_pointer_list_create();
  node_pointer_list_add(ordered_pointers, dawg_root);
  node_pointer_list_add(ordered_pointers, gaddag_root);

  if (output_dawg) {
    set_final_indices(dawg_root, nodes, ordered_pointers);
  }
  if (output_gaddag) {
    set_final_indices(gaddag_root, nodes, ordered_pointers);
  }
  const size_t final_node_count = ordered_pointers->count;
  KWG *kwg = kwg_create_empty();
  kwg_allocate_nodes(kwg, final_node_count);
  copy_nodes(ordered_pointers, nodes, kwg);
  mutable_node_list_destroy(nodes);
  node_pointer_list_destroy(ordered_pointers);
  return kwg;
}
