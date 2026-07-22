#include "word_info_table_maker.h"

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/word_info_table.h"
#include "../util/io_util.h"
#include "kwg_maker.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Pointer trie used during construction; flattened into a WitTrie.
typedef struct PNode {
  struct PNode *child;
  struct PNode *sibling;
  uint32_t *value; // NULL unless this node terminates a word
  uint32_t index;  // assigned during flatten
  MachineLetter tile;
} PNode;

enum { PNODE_BLOCK = 8192 };
typedef struct PBlock {
  struct PBlock *next;
  PNode nodes[PNODE_BLOCK];
} PBlock;

typedef struct PArena {
  PBlock *head;
  int used; // in head block
  uint32_t count;
} PArena;

static PNode *parena_alloc(PArena *arena) {
  if (arena->head == NULL || arena->used == PNODE_BLOCK) {
    PBlock *block = malloc_or_die(sizeof(PBlock));
    block->next = arena->head;
    arena->head = block;
    arena->used = 0;
  }
  PNode *node = &arena->head->nodes[arena->used++];
  node->child = NULL;
  node->sibling = NULL;
  node->value = NULL;
  node->index = 0;
  node->tile = 0;
  arena->count++;
  return node;
}

// Find-or-create the child of `parent` with tile `tile`, keeping children
// sorted by tile.
static PNode *find_or_create_child(PArena *arena, PNode *parent,
                                   MachineLetter tile) {
  PNode **link = &parent->child;
  while (*link != NULL && (*link)->tile < tile) {
    link = &(*link)->sibling;
  }
  if (*link != NULL && (*link)->tile == tile) {
    return *link;
  }
  PNode *node = parena_alloc(arena);
  node->tile = tile;
  node->sibling = *link;
  *link = node;
  return node;
}

static PNode *find_child(const PNode *parent, MachineLetter tile) {
  for (PNode *child = parent->child; child != NULL && child->tile <= tile;
       child = child->sibling) {
    if (child->tile == tile) {
      return child;
    }
  }
  return NULL;
}

static uint32_t pnode_count(const PNode *first_child) {
  uint32_t total = 0;
  for (const PNode *node = first_child; node != NULL; node = node->sibling) {
    total += 1 + pnode_count(node->child);
  }
  return total;
}

// Flatten the pointer trie under `dummy_root` into `out`, copying each
// terminal's `stride` value words. Node 0 is the null node.
static void flatten_trie(PNode *dummy_root, int stride, WitTrie *out) {
  const uint32_t num_nodes = 1 + pnode_count(dummy_root->child);
  out->num_nodes = num_nodes;

  // BFS so each node's siblings get contiguous indices; record index order.
  PNode **queue = malloc_or_die((size_t)num_nodes * sizeof(PNode *));
  const PNode **order = malloc_or_die((size_t)num_nodes * sizeof(PNode *));
  int qhead = 0;
  int qtail = 0;
  uint32_t next_index = 1;
  if (dummy_root->child != NULL) {
    queue[qtail++] = dummy_root->child;
  }
  while (qhead < qtail) {
    for (PNode *node = queue[qhead++]; node != NULL; node = node->sibling) {
      node->index = next_index;
      order[next_index] = node;
      next_index++;
      if (node->child != NULL) {
        queue[qtail++] = node->child;
      }
    }
  }

  out->root = dummy_root->child != NULL ? dummy_root->child->index : 0;
  out->node_tile = calloc_or_die(num_nodes, sizeof(uint8_t));
  out->node_last = calloc_or_die(num_nodes, sizeof(uint8_t));
  out->node_child = calloc_or_die(num_nodes, sizeof(uint32_t));
  out->node_value = malloc_or_die((size_t)num_nodes * sizeof(int32_t));
  for (uint32_t node_idx = 0; node_idx < num_nodes; node_idx++) {
    out->node_value[node_idx] = -1;
  }

  uint32_t num_values = 0;
  for (uint32_t node_idx = 1; node_idx < num_nodes; node_idx++) {
    if (order[node_idx]->value != NULL) {
      num_values++;
    }
  }
  out->num_values = num_values;
  out->values = num_values > 0 ? calloc_or_die((size_t)num_values * stride,
                                               sizeof(uint32_t))
                               : NULL;

  uint32_t value_cursor = 0;
  for (uint32_t node_idx = 1; node_idx < num_nodes; node_idx++) {
    const PNode *node = order[node_idx];
    out->node_tile[node_idx] = node->tile;
    out->node_last[node_idx] = node->sibling == NULL ? 1 : 0;
    out->node_child[node_idx] = node->child != NULL ? node->child->index : 0;
    if (node->value != NULL) {
      memcpy(out->values + (size_t)value_cursor * stride, node->value,
             (size_t)stride * sizeof(uint32_t));
      out->node_value[node_idx] = (int32_t)value_cursor;
      value_cursor++;
    }
  }

  free(queue);
  free(order);
}

WordInfoTable *
make_word_info_table_from_words(const DictionaryWordList *words) {
  PArena arena = {0};
  // A separate trie per key-word length; each root's children are its top list.
  PNode *roots[BOARD_DIM + 1];
  for (int len = 0; len <= BOARD_DIM; len++) {
    roots[len] = parena_alloc(&arena);
  }
  const int num_words = dictionary_word_list_get_count(words);

  // Insert every word into its length's trie and allocate its value row.
  for (int i = 0; i < num_words; i++) {
    const DictionaryWord *dw = dictionary_word_list_get_word(words, i);
    const MachineLetter *letters = dictionary_word_get_word(dw);
    const int len = dictionary_word_get_length(dw);
    if (len < 1 || len > BOARD_DIM) {
      continue;
    }
    PNode *node = roots[len];
    for (int pos = 0; pos < len; pos++) {
      node = find_or_create_child(&arena, node, letters[pos]);
    }
    if (node->value == NULL) {
      node->value =
          calloc_or_die((size_t)wit_stride_for_len(len), sizeof(uint32_t));
    }
  }

  // For each word V and each contiguous substring W that is itself a word, OR
  // V's letter-set into W's value row at slot (|V| - |W|).
  for (int i = 0; i < num_words; i++) {
    const DictionaryWord *dw = dictionary_word_list_get_word(words, i);
    const MachineLetter *v = dictionary_word_get_word(dw);
    const int vlen = dictionary_word_get_length(dw);
    if (vlen < 1 || vlen > BOARD_DIM) {
      continue;
    }
    uint32_t lv = 0;
    for (int k = 0; k < vlen; k++) {
      lv |= 1U << v[k];
    }
    for (int start = 0; start < vlen; start++) {
      const int max_wlen = vlen - start;
      for (int wlen = 1; wlen <= max_wlen; wlen++) {
        const PNode *node = roots[wlen];
        for (int pos = 0; pos < wlen; pos++) {
          node = find_child(node, v[start + pos]);
          if (node == NULL) {
            break;
          }
        }
        if (node != NULL && node->value != NULL) {
          node->value[vlen - wlen] |= lv;
        }
      }
    }
  }

  WordInfoTable *wit = malloc_or_die(sizeof(WordInfoTable));
  wit->name = NULL;
  wit->version = WIT_VERSION;
  for (int len = 0; len <= BOARD_DIM; len++) {
    flatten_trie(roots[len], wit_stride_for_len(len), &wit->tries[len]);
  }

  // Free the construction arena and per-node value rows.
  for (PBlock *block = arena.head; block != NULL;) {
    const int limit = (block == arena.head) ? arena.used : PNODE_BLOCK;
    for (int i = 0; i < limit; i++) {
      free(block->nodes[i].value);
    }
    PBlock *next = block->next;
    free(block);
    block = next;
  }
  return wit;
}

WordInfoTable *make_word_info_table_from_kwg(const KWG *kwg) {
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), words, NULL);
  WordInfoTable *wit = make_word_info_table_from_words(words);
  dictionary_word_list_destroy(words);
  return wit;
}
