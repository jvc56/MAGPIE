#include "kwg_maker.h"

#include <assert.h>
#include <time.h>

#include "../def/cross_set_defs.h"
#include "../ent/config.h"
#include "../ent/conversion_results.h"
#include "../ent/kwg.h"
#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

typedef enum {
  KWG_MAKER_INDEX_ROOT,
  KWG_MAKER_INDEX_REST,
} kwg_maker_index_phase_t;

typedef struct NodeIndexList {
  uint32_t *indices;
  size_t count;
  size_t capacity;
} NodeIndexList;

void node_index_list_initialize(NodeIndexList *list) {
  list->capacity = 5;
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
  mutable_node_list->capacity = 8000000;
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
  // printf("adding nodes->nodes[%zu]\n", nodes->count);
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

uint64_t mutable_node_hash_value(MutableNode *node, MutableNodeList *nodes,
                                 bool check_node) {
  static const uint64_t prime1 = 0xb492b66fbe98f273ULL;
  static const uint64_t prime2 = 0x9ae16a3b2f90404fULL;
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
    hash_with_just_children ^= child_hash * prime1;
  }
  // rotate by one bit to designate the end of the child list
  hash_with_just_children =
      (hash_with_just_children << 1) | (hash_with_just_children >> (64 - 1));

  node->hash_with_just_children = hash_with_just_children;
  node->hash_with_just_children_computed = true;
  if (!check_node) {
    return hash_with_just_children;
  }
  uint64_t hash_with_node = hash_with_just_children * prime2;

  uint8_t ml = node->ml;
  bool accepts = node->accepts;
  hash_with_node ^= 1 + ml;
  if (accepts) {
    hash_with_node ^= 1 << 6;
  }
  node->hash_with_node = hash_with_node;
  node->hash_with_node_computed = true;
  return hash_with_node;
}

bool mutable_node_equals(MutableNode *node_a, MutableNode *node_b,
                         MutableNodeList *nodes, bool check_node) {
  if (check_node) {
    if (node_a->hash_with_node != node_b->hash_with_node) {
      // printf("node_a->hash_value: %llu node_b->hash_value: %llu\n",
      //        node_a->hash_value, node_b->hash_value);
      return false;
    }
    if (node_a->ml != node_b->ml) {
      // printf("node_a->ml: %c node_b->ml: %c\n", 'A' + node_a->ml - 1,
      //        'A' + node_b->ml - 1);
      return false;
    }
    if (node_a->accepts != node_b->accepts) {
      // printf("node_a->accepts: %i node_b->accepts: %i\n", node_a->accepts,
      //        node_b->accepts);
      return false;
    }
  } else {
    if (node_a->hash_with_just_children != node_b->hash_with_just_children) {
      // printf("node_a->hash_value: %llu node_b->hash_value: %llu\n",
      //        node_a->hash_value, node_b->hash_value);
      return false;
    }
  }
  if (node_a->children.count != node_b->children.count) {
    // printf("node_a->children.count: %zu node_b->children.count: %zu\n",
    //        node_a->children.count, node_b->children.count);
    return false;
  }
  for (uint8_t i = 0; i < node_a->children.count; i++) {
    MutableNode *child_a = &nodes->nodes[node_a->children.indices[i]];
    MutableNode *child_b = &nodes->nodes[node_b->children.indices[i]];
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
  node_pointer_list->capacity = 1000000;
  node_pointer_list->nodes =
      malloc_or_die(sizeof(MutableNode *) * node_pointer_list->capacity);
  node_pointer_list->count = 0;
  return node_pointer_list;
}

void node_pointer_list_initialize(NodePointerList *list) {
  list->capacity = 2;
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

void node_hash_table_initialize(NodeHashTable *table, size_t bucket_count) {
  table->bucket_count = bucket_count;
  table->buckets = malloc_or_die(sizeof(NodePointerList) * bucket_count);
  for (size_t i = 0; i < bucket_count; i++) {
    node_pointer_list_initialize(&table->buckets[i]);
  }
}

MutableNode *node_hash_table_find_or_insert(NodeHashTable *table,
                                            MutableNode *node,
                                            MutableNodeList *nodes) {
  uint64_t hash_value = mutable_node_hash_value(node, nodes, false);
  size_t bucket_index = hash_value % table->bucket_count;
  // printf("bucket_index: %zu\n", bucket_index);
  NodePointerList *bucket = &table->buckets[bucket_index];
  for (size_t i = 0; i < bucket->count; i++) {
    MutableNode *candidate = bucket->nodes[i];
    if (mutable_node_equals(node, candidate, nodes, false)) {
      return candidate;
    } else {
      // printf("hash collision\n");
    }
  }
  node_pointer_list_add(bucket, node);
  return node;
}

void set_final_indices(MutableNode *node, MutableNodeList *nodes,
                       NodePointerList *ordered_pointers) {
  // printf("set_final_indices node->ml: %c, ordered_pointers->count: %zu\n",
  //        'A' + node->ml - 1, ordered_pointers->count);
  // printf("node has %zu children\n", node->children.count);
  for (size_t i = 0; i < node->children.count; i++) {
    const int child_index = node->children.indices[i];
    MutableNode *child = &nodes->nodes[child_index];
    child->is_end = (i == (node->children.count - 1));
    child->final_index = ordered_pointers->count;
    node_pointer_list_add(ordered_pointers, child);
  }
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
  // printf("insert_suffix pos: %i, suffix: ", pos);
  // for (int i = pos; i < dictionary_word_get_length(word); i++) {
  //   printf("%c", 'A' + dictionary_word_get_word(word)[i] - 1);
  // }
  // printf("\n");
  const int length = dictionary_word_get_length(word);
  if (pos == length) {
    node->accepts = true;
    return;
  }
  const int ml = dictionary_word_get_word(word)[pos];
  // printf("ml: %c\n", 'A' + ml - 1);
  const uint8_t node_num_children = node->children.count;
  // printf("node_num_children: %d\n", node_num_children);
  for (uint8_t i = 0; i < node_num_children; i++) {
    node = &nodes->nodes[node_index];
    const int child_index = node->children.indices[i];
    const MutableNode *child = &nodes->nodes[node->children.indices[i]];
    if (child->ml == ml) {
      // printf("found child with ml: %c at %d\n", 'A' + ml - 1, i);
      assert(cached_node_indices[pos + 1] == child_index);
      insert_suffix(child_index, nodes, word, pos + 1, cached_node_indices);
      return;
    }
  }
  // printf("adding child for ml: %c\n", 'A' + ml - 1);
  const int child_index = add_child(node_index, nodes, ml);
  cached_node_indices[pos + 1] = child_index;
  insert_suffix(child_index, nodes, word, pos + 1, cached_node_indices);
}

void copy_nodes(NodePointerList *ordered_pointers, MutableNodeList *nodes,
                KWG *kwg) {
  uint32_t *kwg_nodes = kwg_get_mutable_nodes(kwg);
  for (size_t node_idx = 0; node_idx < ordered_pointers->count; node_idx++) {
    // printf("node_idx: %zu\n", node_idx);
    MutableNode *node = ordered_pointers->nodes[node_idx];
    // printf("node->ml: %c\n", 'A' + node->ml - 1);
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
      int original_child_index = children->indices[0];
      uint32_t final_child_index =
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
  uint8_t gaddag_string[MAX_KWG_STRING_LENGTH];
  // First add the word reversed without the separator.
  for (int i = 0; i < length; i++) {
    int source_index = length - i - 1;
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
    const int ml = kwg_tile(kwg, i);
    const int new_node_index = kwg_arc_index(kwg, i);
    bool accepts = kwg_accepts(kwg, i);
    prefix[prefix_length] = ml;
    write_words_aux(kwg, new_node_index, prefix, prefix_length + 1, accepts,
                    words, nodes_reached);
    if (kwg_is_end(kwg, i)) {
      break;
    }
  }
}

void kwg_write_words(const KWG *kwg, int node_index, DictionaryWordList *words,
                     bool *nodes_reached) {
  uint8_t prefix[MAX_KWG_STRING_LENGTH];
  write_words_aux(kwg, node_index, prefix, 0, false, words, nodes_reached);
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
  int cached_node_indices[MAX_KWG_STRING_LENGTH + 1];
  uint8_t last_word[MAX_KWG_STRING_LENGTH];
  int last_word_length = 0;
  for (int i = 0; i < MAX_KWG_STRING_LENGTH; i++) {
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
  }

  printf("nodes->count: %zu\n", nodes->count);

  if (merging == KWG_MAKER_MERGE_EXACT) {
    NodeHashTable table;
    node_hash_table_initialize(&table, 1300021);  // prime
    for (size_t i = 0; i < nodes->count; i++) {
      if (!output_dawg && (i == 0)) {
        continue;
      }
      if (!output_gaddag && (i == 1)) {
        continue;
      }
      // printf("finding or inserting node %zu\n", i);
      MutableNode *node = &nodes->nodes[i];
      MutableNode *match = node_hash_table_find_or_insert(&table, node, nodes);
      if (match == node) {
        // printf("match is the node itself, we inserted something new\n");
        continue;
      }
      assert(match->merged_into == NULL);
      node->merged_into = match;
    }
    int buckets_used = 0;
    size_t max_bucket_size = 0;
    for (size_t i = 0; i < table.bucket_count; i++) {
      NodePointerList *bucket = &table.buckets[i];
      if (bucket->count > 0) {
        buckets_used++;
      }
      if (bucket->count > max_bucket_size) {
        max_bucket_size = bucket->count;
      }
    }
    node_pointer_list_destroy(table.buckets);
    printf("buckets_used: %d max_bucket_size: %zu\n", buckets_used,
           max_bucket_size);
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
  printf("final_node_count: %d\n", final_node_count);
  KWG *kwg = kwg_create_empty();
  kwg_allocate_nodes(kwg, final_node_count);
  copy_nodes(ordered_pointers, nodes, kwg);
  /*
    for (int i = 0; i < final_node_count; i++) {
      const uint32_t node = kwg_get_mutable_nodes(kwg)[i];
      printf("%02x: %08x\n", i, node);
    }
    */
  return kwg;
}

conversion_status_t convert(const Config *config,
                            ConversionResults *conversion_results) {
  LetterDistribution *ld = config_get_ld(config);
  const char *input_filename = config_get_input_filename(config);
  const char *output_filename = config_get_output_filename(config);

  if (input_filename == NULL) {
    return CONVERT_STATUS_INPUT_FILE_ERROR;
  }
  if (output_filename == NULL) {
    return CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE;
  }
  conversion_type_t conversion_type = config_get_conversion_type(config);
  DictionaryWordList *strings = dictionary_word_list_create();
  char line[BOARD_DIM + 2];  // +1 for \n, +1 for \0
  uint8_t mls[BOARD_DIM];
  if ((conversion_type == CONVERT_TEXT2DAWG) ||
      (conversion_type == CONVERT_TEXT2GADDAG) ||
      (conversion_type == CONVERT_TEXT2KWG)) {
    FILE *input_file = fopen(input_filename, "r");
    if (!input_file) {
      return CONVERT_STATUS_INPUT_FILE_ERROR;
    }
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    while (fgets(line, BOARD_DIM + 2, input_file)) {
      const int word_length = string_length(line) - 1;
      line[word_length] = '\0';
      if (word_length > BOARD_DIM) {
        return CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_LONG;
      }
      int mls_length = ld_str_to_mls(ld, line, false, mls, word_length);
      if (mls_length < 0) {
        return CONVERT_STATUS_TEXT_CONTAINS_INVALID_LETTER;
      }
      if (mls_length < 2) {
        return CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_SHORT;
      }
      dictionary_word_list_add_word(strings, mls, mls_length);
    }
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_time = (end_time.tv_sec - start_time.tv_sec) +
                          (end_time.tv_nsec - start_time.tv_nsec) * 1e-9;
    printf("time to read file: %fs\n", elapsed_time);
    printf("loaded %d words\n", dictionary_word_list_get_count(strings));

    kwg_maker_output_t output_type = KWG_MAKER_OUTPUT_DAWG_AND_GADDAG;
    if (conversion_type == CONVERT_TEXT2DAWG) {
      output_type = KWG_MAKER_OUTPUT_DAWG;
    } else if (conversion_type == CONVERT_TEXT2GADDAG) {
      output_type = KWG_MAKER_OUTPUT_GADDAG;
    } 
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    KWG *kwg = make_kwg_from_words(strings, output_type, KWG_MAKER_MERGE_EXACT);
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_time = (end_time.tv_sec - start_time.tv_sec) +
                   (end_time.tv_nsec - start_time.tv_nsec) * 1e-9;
    printf("time to make kwg: %fs\n", elapsed_time);
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    if (!kwg_write_to_file(kwg, output_filename)) {
      printf("failed to write output file: %s\n", output_filename);
      return CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE;
    }
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_time = (end_time.tv_sec - start_time.tv_sec) +
                   (end_time.tv_nsec - start_time.tv_nsec) * 1e-9;
    printf("time to write kwg: %fs\n", elapsed_time);
    conversion_results_set_number_of_strings(
        conversion_results, dictionary_word_list_get_count(strings));
  } else {
    return CONVERT_STATUS_UNIMPLEMENTED_CONVERSION_TYPE;
  }

  return CONVERT_STATUS_SUCCESS;
}