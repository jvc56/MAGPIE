#include "kwg_maker.h"

#include "../def/cross_set_defs.h"

#include "../ent/kwg.h"

#include "../util/string_util.h"
#include "../util/util.h"

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
  uint32_t *indices;
  size_t count;
  size_t capacity;
} NodeIndexList;

void node_index_list_initialize(NodeIndexList *list) {
  list->capacity = KWG_NODE_INDEX_LIST_INITIAL_CAPACITY;
  list->indices = malloc_or_die(sizeof(uint32_t) * list->capacity);
  list->count = 0;
}

void node_index_list_add(NodeIndexList *list, uint32_t index) {
  if (list->count == list->capacity) {
    list->indices =
        realloc_or_die(list->indices, sizeof(uint32_t) * list->capacity * 2);
    list->capacity *= 2;
  }
  list->indices[list->count] = index;
  list->count++;
}

void node_index_list_destroy(NodeIndexList *list) { free(list->indices); }

typedef struct MutableNode {
  uint8_t ml;
  bool accepts;
  bool is_end;
  NodeIndexList children;
  uint64_t hash_with_just_children;
  uint64_t hash_with_node;
  bool hash_with_just_children_computed;
  bool hash_with_node_computed;
  struct MutableNode *merged_into;
  uint8_t merge_offset;
  uint32_t final_index;
} MutableNode;

typedef struct MutableNodeList {
  MutableNode *nodes;
  size_t count;
  size_t capacity;
} MutableNodeList;

MutableNodeList *mutable_node_list_create() {
  MutableNodeList *mutable_node_list = malloc_or_die(sizeof(MutableNodeList));
  mutable_node_list->capacity = KWG_MUTABLE_NODE_LIST_INITIAL_CAPACITY;
  mutable_node_list->nodes =
      malloc_or_die(sizeof(MutableNode) * mutable_node_list->capacity);
  mutable_node_list->count = 0;
  return mutable_node_list;
}

MutableNode *mutable_node_list_add(MutableNodeList *nodes) {
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
  node->hash_with_just_children_computed = false;
  node->hash_with_node_computed = false;
  nodes->count++;
  return node;
}

int mutable_node_list_add_root(MutableNodeList *nodes) {
  const int root_node_index = nodes->count;
  MutableNode *root = mutable_node_list_add(nodes);
  node_index_list_initialize(&root->children);
  return root_node_index;
}

int add_child(int node_index, MutableNodeList *nodes, uint8_t ml) {
  int child_node_index = nodes->count;
  MutableNode *node = &nodes->nodes[node_index];
  node_index_list_add(&node->children, child_node_index);
  MutableNode *child = mutable_node_list_add(nodes);
  child->ml = ml;
  node_index_list_initialize(&child->children);
  return child_node_index;
}

void mutable_node_list_destroy(MutableNodeList *nodes) {
  for (size_t i = 0; i < nodes->count; i++) {
    node_index_list_destroy(&nodes->nodes[i].children);
  }
  free(nodes->nodes);
  free(nodes);
}

uint64_t mutable_node_hash_value(MutableNode *node, MutableNodeList *nodes,
                                 bool check_node) {
  if (check_node) {
    if (node->hash_with_node_computed) {
      return node->hash_with_node;
    }
  } else {
    if (node->hash_with_just_children_computed) {
      return node->hash_with_just_children;
    }
  }
  uint64_t hash_with_just_children = 0;

  for (uint8_t i = 0; i < node->children.count; i++) {
    uint64_t child_hash = 0;
    const int child_index = node->children.indices[i];
    if (child_index != 0) {
      MutableNode *child = &nodes->nodes[node->children.indices[i]];
      child_hash = mutable_node_hash_value(child, nodes, true);
    }
    hash_with_just_children ^= child_hash * KWG_HASH_COMBINING_PRIME_1;
  }
  // rotate by one bit to designate the end of the child list
  hash_with_just_children =
      (hash_with_just_children << 1) | (hash_with_just_children >> (64 - 1));

  node->hash_with_just_children = hash_with_just_children;
  node->hash_with_just_children_computed = true;
  if (!check_node) {
    return hash_with_just_children;
  }
  uint64_t hash_with_node =
      hash_with_just_children * KWG_HASH_COMBINING_PRIME_2;

  const uint8_t ml = node->ml;
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
  node->hash_with_node_computed = true;
  return hash_with_node;
}

bool mutable_node_equals(const MutableNode *node_a, const MutableNode *node_b,
                         const MutableNodeList *nodes, bool check_node) {
  // check_node is false at the root node of a the comparison, we are looking
  // for nodes with different prefixes, including the content of this node,
  // but with identical child subtries. So we only check the letters and accepts
  // within subtries.
  if (check_node) {
    if ((node_a->hash_with_node != node_b->hash_with_node) ||
        (node_a->ml != node_b->ml) || (node_a->accepts != node_b->accepts)) {
      return false;
    }
  }
  if ((node_a->hash_with_just_children != node_b->hash_with_just_children) ||
      (node_a->children.count != node_b->children.count)) {
    return false;
  }
  for (uint8_t i = 0; i < node_a->children.count; i++) {
    const MutableNode *child_a = &nodes->nodes[node_a->children.indices[i]];
    const MutableNode *child_b = &nodes->nodes[node_b->children.indices[i]];
    if (!mutable_node_equals(child_a, child_b, nodes, true)) {
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

NodePointerList *node_pointer_list_create() {
  NodePointerList *node_pointer_list = malloc_or_die(sizeof(NodePointerList));
  node_pointer_list->capacity = KWG_ORDERED_POINTER_LIST_INITIAL_CAPACITY;
  node_pointer_list->nodes =
      malloc_or_die(sizeof(MutableNode *) * node_pointer_list->capacity);
  node_pointer_list->count = 0;
  return node_pointer_list;
}

void node_pointer_list_initialize(NodePointerList *list) {
  list->capacity = KWG_HASH_BUCKET_ITEMS_CAPACITY;
  list->nodes = malloc_or_die(sizeof(MutableNode *) * list->capacity);
  list->count = 0;
}

void node_pointer_list_add(NodePointerList *list, MutableNode *node) {
  if (list->count == list->capacity) {
    list->nodes =
        realloc_or_die(list->nodes, sizeof(MutableNode *) * list->capacity * 2);
    list->capacity *= 2;
  }
  list->nodes[list->count] = node;
  list->count++;
}

void node_pointer_list_destroy(NodePointerList *list) {
  free(list->nodes);
  free(list);
}

typedef struct NodeHashTable {
  NodePointerList *buckets;
  size_t bucket_count;
} NodeHashTable;

void node_hash_table_create(NodeHashTable *table, size_t bucket_count) {
  table->bucket_count = bucket_count;
  table->buckets = malloc_or_die(sizeof(NodePointerList) * bucket_count);
  for (size_t i = 0; i < bucket_count; i++) {
    node_pointer_list_initialize(&table->buckets[i]);
  }
}

void node_hash_table_destroy_buckets(NodeHashTable *table) {
  for (size_t i = 0; i < table->bucket_count; i++) {
    free(table->buckets[i].nodes);
  }
  free(table->buckets);
}

MutableNode *node_hash_table_find_or_insert(NodeHashTable *table,
                                            MutableNode *node,
                                            MutableNodeList *nodes) {
  const uint64_t hash_value = mutable_node_hash_value(node, nodes, false);
  const size_t bucket_index = hash_value % table->bucket_count;
  NodePointerList *bucket = &table->buckets[bucket_index];
  for (size_t i = 0; i < bucket->count; i++) {
    MutableNode *candidate = bucket->nodes[i];
    if (mutable_node_equals(node, candidate, nodes, false)) {
      return candidate;
    }
  }
  node_pointer_list_add(bucket, node);
  return node;
}

void set_final_indices(MutableNode *node, MutableNodeList *nodes,
                       NodePointerList *ordered_pointers) {
  // Add the children in a sequence.
  for (size_t i = 0; i < node->children.count; i++) {
    const int child_index = node->children.indices[i];
    MutableNode *child = &nodes->nodes[child_index];
    child->is_end = (i == (node->children.count - 1));
    child->final_index = ordered_pointers->count;
    node_pointer_list_add(ordered_pointers, child);
  }
  // Then add each of their subtries afterwards.
  for (size_t i = 0; i < node->children.count; i++) {
    const int child_index = node->children.indices[i];
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
  for (uint8_t i = 0; i < node_num_children; i++) {
    node = &nodes->nodes[node_index];
    const int child_index = node->children.indices[i];
    const MutableNode *child = &nodes->nodes[node->children.indices[i]];
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
                KWG *kwg) {
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
      const int original_child_index = children->indices[0];
      const uint32_t final_child_index =
          nodes->nodes[original_child_index].final_index;
      serialized_node |= final_child_index;
    }
    kwg_nodes[node_idx] = serialized_node;
  }
}

void add_gaddag_strings_for_word(const DictionaryWord *word,
                                 DictionaryWordList *gaddag_strings) {
  const uint8_t *raw_word = dictionary_word_get_word(word);
  const int length = dictionary_word_get_length(word);
  uint8_t *gaddag_string =
      malloc_or_die(sizeof(uint8_t) *
                    dictionary_word_list_get_max_word_length(gaddag_strings));
  // First add the word reversed without the separator.
  for (int i = 0; i < length; i++) {
    const int source_index = length - i - 1;
    gaddag_string[i] = raw_word[source_index];
  }
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
  free(gaddag_string);
}

void add_gaddag_strings(const DictionaryWordList *words,
                        DictionaryWordList *gaddag_strings) {
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    add_gaddag_strings_for_word(word, gaddag_strings);
  }
  dictionary_word_list_sort(gaddag_strings);
}

void write_words_aux(const KWG *kwg, int node_index, uint8_t *prefix,
                     int prefix_length, bool accepts, DictionaryWordList *words,
                     bool *nodes_reached) {
  if (accepts) {
    dictionary_word_list_add_word(words, prefix, prefix_length);
  }
  if (node_index == 0) {
    return;
  }
  for (int i = node_index;; i++) {
    if (nodes_reached != NULL) {
      nodes_reached[i] = true;
    }
    const uint32_t node = kwg_node(kwg, i);
    const int ml = kwg_node_tile(node);
    const int new_node_index = kwg_node_arc_index(node);
    const bool accepts = kwg_node_accepts(node);
    prefix[prefix_length] = ml;
    write_words_aux(kwg, new_node_index, prefix, prefix_length + 1, accepts,
                    words, nodes_reached);
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void kwg_write_words(const KWG *kwg, int node_index, DictionaryWordList *words,
                     bool *nodes_reached) {
  uint8_t *prefix = malloc_or_die(
      sizeof(uint8_t) * dictionary_word_list_get_max_word_length(words));
  write_words_aux(kwg, node_index, prefix, 0, false, words, nodes_reached);
  free(prefix);
}

int get_letters_in_common(const DictionaryWord *word, uint8_t *last_word,
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
  memory_copy(last_word, dictionary_word_get_word(word), length);
  return letters_in_common;
}

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
  int max_word_length = dictionary_word_list_get_max_word_length(words);
  int *cached_node_indices = malloc_or_die(sizeof(int) * (max_word_length + 1));

  uint8_t *last_word = malloc_or_die(sizeof(uint8_t) * max_word_length);

  int last_word_length = 0;
  for (int i = 0; i < max_word_length; i++) {
    last_word[i] = 0;
  }
  if (output_dawg) {
    cached_node_indices[0] = dawg_root_node_index;
    for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
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
    DictionaryWordList *gaddag_strings =
        dictionary_word_list_create(max_word_length);
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
    NodeHashTable table;
    node_hash_table_create(&table, KWG_HASH_NUMBER_OF_BUCKETS);
    for (size_t i = 0; i < nodes->count; i++) {
      if (!output_dawg && (i == 0)) {
        continue;
      }
      if (!output_gaddag && (i == 1)) {
        continue;
      }
      MutableNode *node = &nodes->nodes[i];
      MutableNode *match = node_hash_table_find_or_insert(&table, node, nodes);
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
  const int final_node_count = ordered_pointers->count;
  KWG *kwg = kwg_create_empty();
  kwg_allocate_nodes(kwg, final_node_count);
  copy_nodes(ordered_pointers, nodes, kwg);
  mutable_node_list_destroy(nodes);
  node_pointer_list_destroy(ordered_pointers);
  return kwg;
}