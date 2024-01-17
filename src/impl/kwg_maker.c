#include "kwg_maker.h"

#include "../def/cross_set_defs.h"
#include "../ent/kwg.h"
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
  list->capacity = 4;
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
  NodeIndexList *merged_into;
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
  mutable_node_list->capacity = 1000;
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
  node->accepts = false;
  node->is_end = false;
  node->merged_into = NULL;
  nodes->count++;
  return node;
}

MutableNode *mutable_node_list_add_root(MutableNodeList *nodes) {
  MutableNode *root = mutable_node_list_add(nodes);
  root->ml = 0;
  root->is_end = false;
  node_index_list_initialize(&root->children);
  nodes->count++;
  return root;
}

MutableNode *add_child(MutableNode *node, MutableNodeList *nodes, uint8_t ml) {
  const uint8_t index = nodes->count;
  MutableNode *child = mutable_node_list_add(nodes);
  node_index_list_add(&node->children, index);
  child->ml = ml;
  child->accepts = false;
  child->is_end = false;
  node_index_list_initialize(&child->children);
  return child;
}

typedef struct NodePointerList {
  MutableNode **nodes;
  size_t count;
  size_t capacity;
} NodePointerList;

NodePointerList *node_pointer_list_create() {
  NodePointerList *node_pointer_list = malloc_or_die(sizeof(NodePointerList));
  node_pointer_list->capacity = 1000;
  node_pointer_list->nodes =
      malloc_or_die(sizeof(MutableNode *) * node_pointer_list->capacity);
  node_pointer_list->count = 0;
  return node_pointer_list;
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

void set_final_indices(MutableNode *node, MutableNodeList *nodes,
                       NodePointerList *ordered_pointers,
                       const MutableNode *root, kwg_maker_index_phase_t phase) {
  printf(
      "set_final_indices.. node: %c ordered_pointers->count: %zu, phase: %i\n",
      'A' + node->ml - 1, ordered_pointers->count, phase);
  if ((phase == KWG_MAKER_INDEX_ROOT) ||
      ((node != root) && (phase == KWG_MAKER_INDEX_REST))) {
    for (uint8_t i = 0; i < node->children.count; i++) {
      MutableNode *child = &nodes->nodes[node->children.indices[i]];
      child->final_index = ordered_pointers->count;
      if (i == node->children.count - 1) {
        child->is_end = true;
      }
      printf("adding child: %i %c\n", i, ('A' + child->ml - 1));
      node_pointer_list_add(ordered_pointers, child);
    }
  }
  if (node->merged_into != NULL) {
    return;
  }
  if (phase == KWG_MAKER_INDEX_ROOT) {
    return;
  }
  for (uint8_t i = 0; i < node->children.count; i++) {
    MutableNode *child = &nodes->nodes[node->children.indices[i]];
    printf("handling subtrie for child: %i %c\n", i, 'A' + child->ml - 1);
    set_final_indices(child, nodes, ordered_pointers, root, phase);
  }
}

void insert_suffix(MutableNode *node, MutableNodeList *nodes,
                   const DictionaryWord *word, int pos) {
  printf("insert_suffix pos: %i, suffix: ", pos);
  for (int i = pos; i < dictionary_word_get_length(word); i++) {
    printf("%c", 'A' + dictionary_word_get_word(word)[i] - 1);
  }
  printf("\n");
  const int length = dictionary_word_get_length(word);
  if (pos == length) {
    node->accepts = true;
    return;
  }
  const int ml = dictionary_word_get_word(word)[pos];
  MutableNode *child;
  for (uint8_t i = 0; i < node->children.count; i++) {
    child = &nodes->nodes[node->children.indices[i]];
    if (child->ml == ml) {
      printf("found child with ml: %c at %d\n", 'A' + ml - 1, i);
      insert_suffix(child, nodes, word, pos + 1);
      return;
    }
  }
  printf("adding child for ml: %c\n", 'A' + ml - 1);
  child = add_child(node, nodes, ml);
  insert_suffix(child, nodes, word, pos + 1);
}

void copy_nodes(NodePointerList *ordered_pointers, MutableNodeList *nodes,
                KWG *kwg) {
  uint32_t *kwg_nodes = kwg_get_mutable_nodes(kwg);
  for (size_t node_idx = 0; node_idx < ordered_pointers->count; node_idx++) {
    printf("node_idx: %zu\n", node_idx);
    MutableNode *node = ordered_pointers->nodes[node_idx];
    printf("node->ml: %c\n", 'A' + node->ml - 1);
    uint32_t serialized_node = node->ml << KWG_TILE_BIT_OFFSET;
    if (node->accepts) {
      serialized_node |= KWG_NODE_ACCEPTS_FLAG;
    }
    if (node->is_end) {
      serialized_node |= KWG_NODE_IS_END_FLAG;
    }
    if (node->children.count > 0) {
      NodeIndexList *children =
          (node->merged_into == NULL) ? &node->children : node->merged_into;
      serialized_node |= nodes->nodes[children->indices[0]].final_index;
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
  // Add the word with separator pivoting at each position from length-1 to 1.
  for (int separator_pos = length - 1; separator_pos >= 1; separator_pos--) {
    gaddag_string[separator_pos] = SEPARATION_MACHINE_LETTER;
    // We want the rest of the word (forwards) after the separator.
    // Only one letter should need to be moved each time.
    gaddag_string[separator_pos + 1] = raw_word[separator_pos];
    dictionary_word_list_add_word(gaddag_strings, gaddag_string, length + 1);
  }
}

void add_gaddag_strings(const DictionaryWordList *words,
                        DictionaryWordList *gaddag_strings,
                        kwg_maker_merge_t merging) {
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    add_gaddag_strings_for_word(word, gaddag_strings);
  }
  if (merging != KWG_MAKER_MERGE_UNORDERED_SUBLIST) {
    dictionary_word_list_sort(gaddag_strings);
  }
}

KWG *make_kwg_from_words(const DictionaryWordList *words,
                         kwg_maker_output_t output, kwg_maker_merge_t merging) {
  const bool output_dawg = (output == KWG_MAKER_OUTPUT_DAWG) ||
                           (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);
  const bool output_gaddag = (output == KWG_MAKER_OUTPUT_GADDAG) ||
                             (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);
  MutableNodeList *nodes = mutable_node_list_create();
  MutableNode *dawg_root = mutable_node_list_add_root(nodes);
  if (output_dawg) {
    for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
      printf("i: %d word: ", i);
      const DictionaryWord *word = dictionary_word_list_get_word(words, i);
      for (int k = 0; k < dictionary_word_get_length(word); k++) {
        printf("%c", 'A' + dictionary_word_get_word(word)[k] - 1);
      }
      printf("\n");
      insert_suffix(dawg_root, nodes, word, 0);
    }
  }

  MutableNode *gaddag_root = mutable_node_list_add_root(nodes);
  if (output_gaddag) {
    DictionaryWordList *gaddag_strings = dictionary_word_list_create();
    add_gaddag_strings(words, gaddag_strings, merging);
    for (int i = 0; i < dictionary_word_list_get_count(gaddag_strings); i++) {
      printf("i: %d gaddag_string: ", i);
      const DictionaryWord *gaddag_string =
          dictionary_word_list_get_word(gaddag_strings, i);
      for (int k = 0; k < dictionary_word_get_length(gaddag_string); k++) {
        printf("%c", 'A' + dictionary_word_get_word(gaddag_string)[k] - 1);
      }
      printf("\n");
      insert_suffix(gaddag_root, nodes, gaddag_string, 0);
    }
  }

  NodePointerList *ordered_pointers = node_pointer_list_create();
  if (output_dawg) {
    node_pointer_list_add(ordered_pointers, dawg_root);
  }
  node_pointer_list_add(ordered_pointers, gaddag_root);

  if (output_dawg) {
    set_final_indices(dawg_root, nodes, ordered_pointers, dawg_root,
                      KWG_MAKER_INDEX_ROOT);
  }
  if (output_gaddag) {
    set_final_indices(gaddag_root, nodes, ordered_pointers, gaddag_root,
                      KWG_MAKER_INDEX_ROOT);
  }
  if (output_dawg) {
    set_final_indices(dawg_root, nodes, ordered_pointers, dawg_root,
                      KWG_MAKER_INDEX_REST);
  }
  if (output_gaddag) {
    set_final_indices(gaddag_root, nodes, ordered_pointers, gaddag_root,
                      KWG_MAKER_INDEX_REST);
  }
  const int final_node_count = ordered_pointers->count;
  printf("final_node_count: %d\n", final_node_count);
  KWG *kwg = kwg_create_empty();
  kwg_allocate_nodes(kwg, final_node_count);
  copy_nodes(ordered_pointers, nodes, kwg);
  for (int i = 0; i < final_node_count; i++) {
    const uint32_t node = kwg_get_mutable_nodes(kwg)[i];
    printf("%02x: %08x\n", i, node);
  }
  return kwg;
}