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

// The KWG data structure was originally developed in wolges.
// For details see: https://github.com/andy-k/wolges/blob/main/details.txt
//
// This implementation uses a wolges-style approach:
// - Compact 12-byte State struct with sibling chains (no child arrays)
// - Bottom-up construction with transition stack
// - Immediate deduplication during state creation
//
// States use arc_index (first child) and next_index (next sibling) to form
// a linked structure, avoiding explicit child arrays.

// ============================================================================
// Compact State-based KWG Builder (wolges-style)
// ============================================================================

// Compact state: 12 bytes (vs 64-byte MutableNode)
// Uses sibling chains instead of child arrays
typedef struct State {
  uint8_t tile;
  uint8_t accepts;     // bool stored as uint8_t for packing
  uint32_t arc_index;  // Index of first child state (0 = no children)
  uint32_t next_index; // Index of next sibling state (0 = last sibling)
} State;

// Transition: temporary structure used during tree construction
// Stored on a stack, converted to States when we backtrack
typedef struct Transition {
  uint8_t tile;
  uint8_t accepts;
  uint32_t arc_index; // Filled when children are finalized
} Transition;

// Growable list of states
typedef struct StateList {
  State *states;
  size_t count;
  size_t capacity;
} StateList;

// Hash table for state deduplication
// Maps State content to state index
typedef struct StateHashTable {
  uint32_t *bucket_heads;  // Head of chain for each bucket
  uint32_t *next_in_chain; // Next state in chain (indexed by state index)
  size_t num_buckets;
  size_t node_capacity;
} StateHashTable;

// Transition stack for building the trie
typedef struct TransitionStack {
  Transition *transitions;
  size_t *depth_markers; // Stack of indices marking where each depth starts
  uint32_t *
      sibling_heads; // Head of sibling chain at each depth (from previous pops)
  size_t trans_count;
  size_t trans_capacity;
  size_t depth;
  size_t depth_capacity;
} TransitionStack;

static inline void state_list_create(StateList *list, size_t initial_capacity) {
  list->capacity = initial_capacity;
  list->states = malloc_or_die(sizeof(State) * initial_capacity);
  list->count = 0;
  // State 0 is the "null" state (no children/siblings)
  list->states[0].tile = 0;
  list->states[0].accepts = 0;
  list->states[0].arc_index = 0;
  list->states[0].next_index = 0;
  list->count = 1;
}

static inline void state_list_destroy(StateList *list) { free(list->states); }

static inline void state_list_ensure_capacity(StateList *list,
                                              size_t required) {
  if (required <= list->capacity) {
    return;
  }
  size_t new_cap = list->capacity;
  while (new_cap < required) {
    new_cap *= 2;
  }
  list->states = realloc_or_die(list->states, sizeof(State) * new_cap);
  list->capacity = new_cap;
}

static inline uint32_t state_list_add(StateList *list, uint8_t tile,
                                      uint8_t accepts, uint32_t arc_index,
                                      uint32_t next_index) {
  state_list_ensure_capacity(list, list->count + 1);
  uint32_t idx = list->count;
  list->states[idx].tile = tile;
  list->states[idx].accepts = accepts;
  list->states[idx].arc_index = arc_index;
  list->states[idx].next_index = next_index;
  list->count++;
  return idx;
}

static inline void state_hash_table_create(StateHashTable *table,
                                           size_t num_buckets,
                                           size_t node_capacity) {
  table->num_buckets = num_buckets;
  table->node_capacity = node_capacity;
  table->bucket_heads = malloc_or_die(sizeof(uint32_t) * num_buckets);
  table->next_in_chain = malloc_or_die(sizeof(uint32_t) * node_capacity);
  for (size_t bucket_idx = 0; bucket_idx < num_buckets; bucket_idx++) {
    table->bucket_heads[bucket_idx] = 0; // 0 = empty (state 0 is null state)
  }
}

static inline void state_hash_table_destroy(StateHashTable *table) {
  free(table->bucket_heads);
  free(table->next_in_chain);
}

static inline void state_hash_table_ensure_capacity(StateHashTable *table,
                                                    size_t required) {
  if (required <= table->node_capacity) {
    return;
  }
  size_t new_cap = table->node_capacity;
  while (new_cap < required) {
    new_cap *= 2;
  }
  table->next_in_chain =
      realloc_or_die(table->next_in_chain, sizeof(uint32_t) * new_cap);
  table->node_capacity = new_cap;
}

// Hash a state by its content
static inline uint64_t state_hash(const State *s) {
  // Combine all fields including next_index
  uint64_t h = s->tile;
  h ^= (uint64_t)s->accepts << 8;
  h ^= (uint64_t)s->arc_index << 16;
  h ^= (uint64_t)s->next_index * KWG_HASH_COMBINING_PRIME;
  return h;
}

// Check if two states are equal
static inline bool state_equals(const State *a, const State *b) {
  return a->tile == b->tile && a->accepts == b->accepts &&
         a->arc_index == b->arc_index && a->next_index == b->next_index;
}

// Find or insert a state, returning its index
// If an equivalent state exists, returns its index
// Otherwise adds the state and returns its new index
static inline uint32_t
state_hash_table_find_or_insert(StateHashTable *table, StateList *list,
                                uint8_t tile, uint8_t accepts,
                                uint32_t arc_index, uint32_t next_index) {
  State candidate = {tile, accepts, arc_index, next_index};
  uint64_t h = state_hash(&candidate);
  size_t bucket = h % table->num_buckets;

  // Search chain for existing match
  uint32_t idx = table->bucket_heads[bucket];
  while (idx != 0) {
    if (state_equals(&list->states[idx], &candidate)) {
      return idx; // Found existing
    }
    idx = table->next_in_chain[idx];
  }

  // Not found - add new state
  uint32_t new_idx = state_list_add(list, tile, accepts, arc_index, next_index);
  state_hash_table_ensure_capacity(table, new_idx + 1);
  table->next_in_chain[new_idx] = table->bucket_heads[bucket];
  table->bucket_heads[bucket] = new_idx;
  return new_idx;
}

static inline void transition_stack_create(TransitionStack *stack,
                                           size_t trans_cap, size_t depth_cap) {
  stack->transitions = malloc_or_die(sizeof(Transition) * trans_cap);
  stack->depth_markers = malloc_or_die(sizeof(size_t) * depth_cap);
  stack->sibling_heads = malloc_or_die(sizeof(uint32_t) * depth_cap);
  stack->trans_count = 0;
  stack->trans_capacity = trans_cap;
  stack->depth = 0;
  stack->depth_capacity = depth_cap;
  for (size_t depth_idx = 0; depth_idx < depth_cap; depth_idx++) {
    stack->sibling_heads[depth_idx] = 0;
  }
}

static inline void transition_stack_destroy(TransitionStack *stack) {
  free(stack->transitions);
  free(stack->depth_markers);
  free(stack->sibling_heads);
}

static inline void transition_stack_ensure_capacity(TransitionStack *stack) {
  if (stack->trans_count >= stack->trans_capacity) {
    stack->trans_capacity *= 2;
    stack->transitions = realloc_or_die(
        stack->transitions, sizeof(Transition) * stack->trans_capacity);
  }
  if (stack->depth >= stack->depth_capacity) {
    size_t old_cap = stack->depth_capacity;
    stack->depth_capacity *= 2;
    stack->depth_markers = realloc_or_die(
        stack->depth_markers, sizeof(size_t) * stack->depth_capacity);
    stack->sibling_heads = realloc_or_die(
        stack->sibling_heads, sizeof(uint32_t) * stack->depth_capacity);
    // Initialize new sibling_heads entries
    for (size_t depth_idx = old_cap; depth_idx < stack->depth_capacity;
         depth_idx++) {
      stack->sibling_heads[depth_idx] = 0;
    }
  }
}

// Push a new transition onto the stack
static inline void transition_stack_push(TransitionStack *stack, uint8_t tile) {
  transition_stack_ensure_capacity(stack);
  stack->depth_markers[stack->depth] = stack->trans_count;
  stack->depth++;
  stack->transitions[stack->trans_count].tile = tile;
  stack->transitions[stack->trans_count].accepts = 0;
  stack->transitions[stack->trans_count].arc_index = 0;
  stack->trans_count++;
}

// Pop transitions and create a deduplicated state chain
// Returns the index of the first state in the chain (head of sibling list)
static inline uint32_t transition_stack_pop(TransitionStack *stack,
                                            StateList *states,
                                            StateHashTable *table) {
  // Get the existing sibling chain at this level (from previous pops under same
  // parent)
  size_t popping_depth = stack->depth;
  uint32_t next_index = stack->sibling_heads[popping_depth];

  stack->depth--;
  size_t start = stack->depth_markers[stack->depth];

  // Create states in reverse order, linking to existing siblings
  for (size_t trans_idx = stack->trans_count; trans_idx > start; trans_idx--) {
    const Transition *t = &stack->transitions[trans_idx - 1];
    next_index = state_hash_table_find_or_insert(
        table, states, t->tile, t->accepts, t->arc_index, next_index);
  }

  // Store the new chain head for future siblings at this level
  stack->sibling_heads[popping_depth] = next_index;

  // Reset the child level's sibling chain (we're done with this subtree)
  if (popping_depth + 1 < stack->depth_capacity) {
    stack->sibling_heads[popping_depth + 1] = 0;
  }

  // Update the arc_index of the transition that will point to these children
  if (start > 0) {
    stack->transitions[start - 1].arc_index = next_index;
  }

  stack->trans_count = start;
  return next_index;
}

// Mark the current transition as accepting (word ends here)
static inline void transition_stack_mark_accepts(TransitionStack *stack) {
  if (stack->trans_count > 0) {
    stack->transitions[stack->trans_count - 1].accepts = 1;
  }
}

// Build a DAWG/GADDAG using the transition stack approach
// words must be sorted
// Returns the index of the first state in the root's child chain
static uint32_t build_dawg_from_sorted_words(const DictionaryWordList *words,
                                             StateList *states,
                                             StateHashTable *table,
                                             TransitionStack *stack) {
  const int word_count = dictionary_word_list_get_count(words);
  if (word_count == 0) {
    return 0;
  }

  MachineLetter prev_word[MAX_KWG_STRING_LENGTH];
  int prev_len = 0;
  uint32_t root_arc = 0; // Will hold the final root arc_index

  for (int w = 0; w < word_count; w++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, w);
    const MachineLetter *letters = dictionary_word_get_word(word);
    const int len = dictionary_word_get_length(word);

    // Find common prefix length with previous word
    int common = 0;
    int min_len = (len < prev_len) ? len : prev_len;
    while (common < min_len && letters[common] == prev_word[common]) {
      common++;
    }

    // Pop transitions for the non-common suffix of the previous word
    while ((int)stack->depth > common) {
      root_arc = transition_stack_pop(stack, states, table);
    }

    // Push transitions for the new suffix
    for (int letter_idx = common; letter_idx < len; letter_idx++) {
      transition_stack_push(stack, letters[letter_idx]);
    }

    // Mark the last letter as accepting
    transition_stack_mark_accepts(stack);

    // Remember this word for next iteration
    memcpy(prev_word, letters, len);
    prev_len = len;
  }

  // Pop all remaining transitions to finalize
  while (stack->depth > 0) {
    root_arc = transition_stack_pop(stack, states, table);
  }

  return root_arc;
}

// Entry in the output queue: a state and where it's placed in the output
typedef struct {
  uint32_t state_idx;  // Index in states array
  uint32_t output_idx; // Index in output KWG
  uint32_t chain_base; // Base index of this sibling chain in output
  bool is_end;         // True if this is the last sibling in output order
} OutputEntry;

// BFS entry for serialization
typedef struct {
  uint32_t arc_index;
  uint32_t output_base;
} SerializeBFSEntry;

// Context for BFS serialization (passed to helper function)
typedef struct {
  const StateList *states;
  OutputEntry **entries;
  size_t *entries_count;
  size_t *entries_capacity;
  SerializeBFSEntry **bfs_queue;
  size_t *bfs_tail;
  bool *visited;
} SerializeContext;

// Helper: process a sibling chain starting at head, placing at output_base.
// Adds ALL states to entries (including previously-serialized ones, as
// duplicates). Note: sibling chains are built in reverse order (Z->Y->...->A),
// but we need to serialize in alphabetical order (A, B, ..., Z) for KWG
// traversal. So we assign output positions in reverse: first in chain gets
// highest index. The chain HEAD is placed last in output, so it has
// is_end=true. IMPORTANT: Each sibling group MUST be consecutive in output (KWG
// format requirement), so we duplicate states that appear in multiple sibling
// chains.
static inline void process_chain_and_queue(SerializeContext *ctx,
                                           uint32_t head_state,
                                           uint32_t out_base,
                                           uint32_t chain_length) {
  if (head_state == 0) {
    return;
  }
  uint32_t pos = chain_length;
  bool first = true;
  for (uint32_t p = head_state; p != 0; p = ctx->states->states[p].next_index) {
    pos--;
    if (*ctx->entries_count >= *ctx->entries_capacity) {
      *ctx->entries_capacity *= 2;
      *ctx->entries = realloc_or_die(
          *ctx->entries, sizeof(OutputEntry) * (*ctx->entries_capacity));
      *ctx->bfs_queue =
          realloc_or_die(*ctx->bfs_queue,
                         sizeof(SerializeBFSEntry) * (*ctx->entries_capacity));
    }
    (*ctx->entries)[*ctx->entries_count].state_idx = p;
    (*ctx->entries)[*ctx->entries_count].output_idx = out_base + pos;
    (*ctx->entries)[*ctx->entries_count].chain_base = out_base;
    (*ctx->entries)[*ctx->entries_count].is_end =
        first; // first in iteration = last in output
    (*ctx->entries_count)++;
    first = false;
    // Queue children for BFS if not already visited
    uint32_t children = ctx->states->states[p].arc_index;
    if (children != 0 && !ctx->visited[children]) {
      ctx->visited[children] = true;
      (*ctx->bfs_queue)[*ctx->bfs_tail].arc_index = children;
      (*ctx->bfs_queue)[*ctx->bfs_tail].output_base = UINT32_MAX; // placeholder
      (*ctx->bfs_tail)++;
    }
  }
}

// Serialize states to KWG format.
// Key insight: siblings must be consecutive in output. Due to deduplication,
// a single state may be part of multiple sibling chains. We handle this by
// serializing each sibling chain independently, potentially duplicating states
// in the output.
static void serialize_states_to_kwg(const StateList *states, uint32_t dawg_root,
                                    uint32_t gaddag_root, KWG *kwg) {
  // First pass: count output nodes needed by traversing all reachable chains.
  // Use visited array to track which chain heads have been queued for BFS.
  size_t max_states = states->count;
  bool *visited = malloc_or_die(sizeof(bool) * max_states);
  for (size_t state_idx = 0; state_idx < max_states; state_idx++) {
    visited[state_idx] = false;
  }

  // Count total output nodes and collect output entries
  size_t output_count = 2;                  // Reserve 0 and 1 for root pointers
  size_t entries_capacity = max_states * 2; // May need duplicates
  OutputEntry *entries = malloc_or_die(sizeof(OutputEntry) * entries_capacity);
  size_t entries_count = 0;

  // Queue for BFS: stores (arc_index to process, output_base for its children)
  SerializeBFSEntry *bfs_queue =
      malloc_or_die(sizeof(SerializeBFSEntry) * entries_capacity);
  size_t bfs_head = 0;
  size_t bfs_tail = 0;

  // Set up serialization context for helper function
  SerializeContext ctx = {.states = states,
                          .entries = &entries,
                          .entries_count = &entries_count,
                          .entries_capacity = &entries_capacity,
                          .bfs_queue = &bfs_queue,
                          .bfs_tail = &bfs_tail,
                          .visited = visited};

  // Track base indices for root chains (used for root pointers)
  uint32_t dawg_base = 0;
  uint32_t gaddag_base = 0;

  // First: count and queue DAWG root chain
  if (dawg_root != 0) {
    uint32_t chain_len = 0;
    for (uint32_t p = dawg_root; p != 0; p = states->states[p].next_index) {
      chain_len++;
    }
    dawg_base = output_count; // Store base for root pointer
    process_chain_and_queue(&ctx, dawg_root, output_count, chain_len);
    output_count += chain_len;
    visited[dawg_root] = true;
  }

  // Then: count and queue GADDAG root chain
  if (gaddag_root != 0 && !visited[gaddag_root]) {
    uint32_t chain_len = 0;
    for (uint32_t p = gaddag_root; p != 0; p = states->states[p].next_index) {
      chain_len++;
    }
    gaddag_base = output_count; // Store base for root pointer
    process_chain_and_queue(&ctx, gaddag_root, output_count, chain_len);
    output_count += chain_len;
    visited[gaddag_root] = true;
  }

  // BFS: process queued children
  while (bfs_head < bfs_tail) {
    SerializeBFSEntry entry = bfs_queue[bfs_head++];
    uint32_t chain_head = entry.arc_index;

    // Count all siblings in this chain
    uint32_t chain_len = 0;
    for (uint32_t p = chain_head; p != 0; p = states->states[p].next_index) {
      chain_len++;
    }

    // Assign output positions and queue
    uint32_t base = output_count;
    output_count += chain_len;

    // Re-iterate to create entries and queue children
    // Note: assign in reverse order (chain is Z->Y->...->A, want A at lowest
    // index) The chain HEAD is placed last in output, so it has is_end=true.
    uint32_t pos = chain_len;
    bool first = true;
    for (uint32_t p = chain_head; p != 0; p = states->states[p].next_index) {
      pos--;
      if (entries_count >= entries_capacity) {
        entries_capacity *= 2;
        entries =
            realloc_or_die(entries, sizeof(OutputEntry) * entries_capacity);
        bfs_queue = realloc_or_die(bfs_queue, sizeof(SerializeBFSEntry) *
                                                  entries_capacity);
      }
      entries[entries_count].state_idx = p;
      entries[entries_count].output_idx = base + pos;
      entries[entries_count].chain_base = base;
      entries[entries_count].is_end =
          first; // first in iteration = last in output
      entries_count++;
      first = false;

      // Queue children for BFS if not already visited
      uint32_t children = states->states[p].arc_index;
      if (children != 0 && !visited[children]) {
        visited[children] = true;
        bfs_queue[bfs_tail].arc_index = children;
        bfs_queue[bfs_tail].output_base = UINT32_MAX;
        bfs_tail++;
      }
    }
  }

  // Build a map from chain_head to chain_base.
  // The chain_head is the state that was queued (its arc_index from a parent).
  // Each entry stores its chain_base, so we just need to record the base for
  // each head.
  uint32_t *chain_base_map = malloc_or_die(sizeof(uint32_t) * max_states);
  for (size_t state_idx = 0; state_idx < max_states; state_idx++) {
    chain_base_map[state_idx] = UINT32_MAX;
  }
  // Record chain_base ONLY for entries where the state is the chain head.
  // The chain head is the first state in iteration order (which gets
  // is_end=true). This is critical because a state can be part of multiple
  // chains:
  // - As the chain head (e.g., I as Q's only child)
  // - As a non-head sibling (e.g., I in X's children U→I)
  // We must record the base for when the state IS the chain head.
  for (size_t entry_idx = 0; entry_idx < entries_count; entry_idx++) {
    // Only record for entries where this state is the chain head (is_end=true)
    uint32_t sidx = entries[entry_idx].state_idx;
    if (entries[entry_idx].is_end && chain_base_map[sidx] == UINT32_MAX) {
      chain_base_map[sidx] = entries[entry_idx].chain_base;
    }
  }

  // Allocate KWG nodes
  kwg_allocate_nodes(kwg, output_count);
  uint32_t *kwg_nodes = kwg_get_mutable_nodes(kwg);

  // Serialize root pointers (indices 0 and 1)
  // Use the base index where we placed the alphabetically-first sibling
  kwg_nodes[0] = dawg_base | KWG_NODE_IS_END_FLAG;
  kwg_nodes[1] = gaddag_base | KWG_NODE_IS_END_FLAG;

  // Serialize each entry
  for (size_t entry_idx = 0; entry_idx < entries_count; entry_idx++) {
    uint32_t sidx = entries[entry_idx].state_idx;
    uint32_t oidx = entries[entry_idx].output_idx;
    const State *s = &states->states[sidx];

    uint32_t node = ((uint32_t)s->tile) << KWG_TILE_BIT_OFFSET;
    if (s->accepts) {
      node |= KWG_NODE_ACCEPTS_FLAG;
    }
    // is_end: true if this is the last sibling in output order
    if (entries[entry_idx].is_end) {
      node |= KWG_NODE_IS_END_FLAG;
    }
    // arc: point to chain base (alphabetically-first child in output order)
    if (s->arc_index != 0) {
      uint32_t child_output = chain_base_map[s->arc_index];
      node |= child_output;
    }
    kwg_nodes[oidx] = node;
  }

  free(visited);
  free(entries);
  free(bfs_queue);
  free(chain_base_map);
}

// Fast KWG builder using compact states and transition stack
KWG *make_kwg_from_words_fast(const DictionaryWordList *words,
                              kwg_maker_output_t output) {
  const bool output_dawg = (output == KWG_MAKER_OUTPUT_DAWG) ||
                           (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);
  const bool output_gaddag = (output == KWG_MAKER_OUTPUT_GADDAG) ||
                             (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);

  const int words_count = dictionary_word_list_get_count(words);

  // Estimate capacity (similar to before)
  const size_t estimated_states = (size_t)words_count * 8 + 100;
  const size_t num_buckets = estimated_states * 2 + 1;

  StateList states;
  state_list_create(&states, estimated_states);

  StateHashTable table;
  state_hash_table_create(&table, num_buckets, estimated_states);

  TransitionStack stack;
  transition_stack_create(&stack, (size_t)MAX_KWG_STRING_LENGTH * 2,
                          (size_t)MAX_KWG_STRING_LENGTH + 1);

  uint32_t dawg_root = 0;
  uint32_t gaddag_root = 0;

  // Build DAWG
  if (output_dawg) {
    dawg_root = build_dawg_from_sorted_words(words, &states, &table, &stack);
  }

  // Build GADDAG
  if (output_gaddag) {
    // Count total GADDAG strings: each word of length L produces L strings
    int total_gaddag_strings = 0;
    for (int word_idx = 0; word_idx < words_count; word_idx++) {
      const DictionaryWord *word =
          dictionary_word_list_get_word(words, word_idx);
      total_gaddag_strings += dictionary_word_get_length(word);
    }

    // Pre-allocate exact capacity to avoid reallocs
    DictionaryWordList *gaddag_strings =
        dictionary_word_list_create_with_capacity(total_gaddag_strings);
    for (int word_idx = 0; word_idx < words_count; word_idx++) {
      const DictionaryWord *word =
          dictionary_word_list_get_word(words, word_idx);
      const MachineLetter *raw_word = dictionary_word_get_word(word);
      const int length = dictionary_word_get_length(word);
      MachineLetter gaddag_string[MAX_KWG_STRING_LENGTH] = {0};

      // Add reversed word (no separator)
      for (int letter_idx = 0; letter_idx < length; letter_idx++) {
        gaddag_string[letter_idx] = raw_word[length - letter_idx - 1];
      }
      dictionary_word_list_add_word(gaddag_strings, gaddag_string, length);

      // Add pivot forms: for "CARE" -> "RAC@E", "AC@RE", "C@ARE"
      for (int sep_pos = length - 1; sep_pos >= 1; sep_pos--) {
        for (int letter_idx = 0; letter_idx < sep_pos; letter_idx++) {
          gaddag_string[letter_idx] = raw_word[sep_pos - letter_idx - 1];
        }
        gaddag_string[sep_pos] = SEPARATION_MACHINE_LETTER;
        for (int letter_idx = sep_pos; letter_idx < length; letter_idx++) {
          gaddag_string[sep_pos + 1 + (letter_idx - sep_pos)] =
              raw_word[letter_idx];
        }
        dictionary_word_list_add_word(gaddag_strings, gaddag_string,
                                      length + 1);
      }
    }
    dictionary_word_list_sort(gaddag_strings);

    // Reset stack for GADDAG building
    stack.trans_count = 0;
    stack.depth = 0;
    for (size_t depth_idx = 0; depth_idx < stack.depth_capacity; depth_idx++) {
      stack.sibling_heads[depth_idx] = 0;
    }

    gaddag_root =
        build_dawg_from_sorted_words(gaddag_strings, &states, &table, &stack);
    dictionary_word_list_destroy(gaddag_strings);
  }

  // Serialize to KWG format
  KWG *kwg = kwg_create_empty();
  serialize_states_to_kwg(&states, dawg_root, gaddag_root, kwg);

  transition_stack_destroy(&stack);
  state_hash_table_destroy(&table);
  state_list_destroy(&states);

  return kwg;
}

// ============================================================================
// Legacy MutableNode-based implementation (kept for reference/comparison)
// ============================================================================

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

// Arena allocator for child index arrays (avoids individual malloc calls)
// Uses a linked list of blocks to avoid realloc invalidating existing pointers
typedef struct ArenaBlock {
  uint32_t *buffer;
  size_t capacity;
  size_t used;
  struct ArenaBlock *next;
} ArenaBlock;

typedef struct IndexArena {
  ArenaBlock *head;
  ArenaBlock *current;
  size_t block_size; // Size for new blocks
} IndexArena;

static inline ArenaBlock *arena_block_create(size_t capacity) {
  ArenaBlock *block = malloc_or_die(sizeof(ArenaBlock));
  block->buffer = malloc_or_die(sizeof(uint32_t) * capacity);
  block->capacity = capacity;
  block->used = 0;
  block->next = NULL;
  return block;
}

static inline void index_arena_create(IndexArena *arena,
                                      size_t initial_capacity) {
  arena->head = arena_block_create(initial_capacity);
  arena->current = arena->head;
  arena->block_size = initial_capacity;
}

static inline void index_arena_destroy(IndexArena *arena) {
  ArenaBlock *block = arena->head;
  while (block != NULL) {
    ArenaBlock *next = block->next;
    free(block->buffer);
    free(block);
    block = next;
  }
}

static inline uint32_t *index_arena_alloc(IndexArena *arena, size_t count) {
  // If current block doesn't have enough space, allocate a new block
  if (arena->current->used + count > arena->current->capacity) {
    size_t new_capacity = arena->block_size;
    if (count > new_capacity) {
      new_capacity =
          count * 2; // Ensure block is big enough for this allocation
    }
    ArenaBlock *new_block = arena_block_create(new_capacity);
    arena->current->next = new_block;
    arena->current = new_block;
  }
  uint32_t *result = arena->current->buffer + arena->current->used;
  arena->current->used += count;
  return result;
}

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

// Arena-aware version - allocates from arena instead of malloc
static inline void node_index_list_add_arena(NodeIndexList *list,
                                             uint32_t index,
                                             IndexArena *arena) {
  if (list->count == list->capacity) {
    size_t new_capacity = (list->capacity == 0) ? 4 : list->capacity * 2;
    uint32_t *new_indices = index_arena_alloc(arena, new_capacity);
    if (list->count > 0) {
      const uint32_t *old_indices = node_index_list_get_const_indices(list);
      memcpy(new_indices, old_indices, sizeof(uint32_t) * list->count);
    }
    list->indices = new_indices;
    list->capacity = new_capacity;
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

// Sentinel value indicating node is not merged
#define NODE_NOT_MERGED UINT32_MAX

typedef struct MutableNode {
  MachineLetter ml;
  bool accepts;
  bool is_end;
  NodeIndexList children;
  uint64_t hash_with_just_children;
  uint64_t hash_with_node;
  uint32_t merged_into_index; // Index of node this is merged into, or
                              // NODE_NOT_MERGED
  uint8_t merge_offset;
  uint32_t final_index;
} MutableNode;

typedef struct MutableNodeList {
  MutableNode *nodes;
  size_t count;
  size_t capacity;
  IndexArena *arena; // Optional arena for child index allocation
} MutableNodeList;

// Create with arena for child index allocation
static inline MutableNodeList *
mutable_node_list_create_with_arena(size_t node_capacity,
                                    size_t arena_capacity) {
  MutableNodeList *mutable_node_list = malloc_or_die(sizeof(MutableNodeList));
  mutable_node_list->capacity = node_capacity;
  mutable_node_list->nodes =
      malloc_or_die(sizeof(MutableNode) * mutable_node_list->capacity);
  mutable_node_list->count = 0;
  mutable_node_list->arena = malloc_or_die(sizeof(IndexArena));
  index_arena_create(mutable_node_list->arena, arena_capacity);
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
  node->merged_into_index = NODE_NOT_MERGED;
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

// Arena-aware version - allocates child indices from arena
static inline int add_child_arena(uint32_t node_index, MutableNodeList *nodes,
                                  MachineLetter ml) {
  const size_t child_node_index = nodes->count;
  MutableNode *node = &nodes->nodes[node_index];
  node_index_list_add_arena(&node->children, child_node_index, nodes->arena);
  MutableNode *child = mutable_node_list_add(nodes);
  child->ml = ml;
  node_index_list_initialize(&child->children);
  return (int)child_node_index;
}

static inline void mutable_node_list_destroy(MutableNodeList *nodes) {
  // If arena is used, all child indices are in the arena - no individual frees
  // needed
  if (nodes->arena == NULL) {
    for (size_t node_idx = 0; node_idx < nodes->count; node_idx++) {
      node_index_list_destroy(&nodes->nodes[node_idx].children);
    }
  } else {
    index_arena_destroy(nodes->arena);
    free(nodes->arena);
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

static inline NodePointerList *
node_pointer_list_create_with_capacity(size_t capacity) {
  NodePointerList *node_pointer_list = malloc_or_die(sizeof(NodePointerList));
  node_pointer_list->capacity = capacity;
  node_pointer_list->nodes = malloc_or_die(sizeof(MutableNode *) * capacity);
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
    if (child->merged_into_index != NODE_NOT_MERGED) {
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
  for (MachineLetter child_pos = 0; child_pos < node_num_children;
       child_pos++) {
    node = &nodes->nodes[node_index];
    const uint32_t child_index = get_child_index(node, child_pos);
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

// Arena-aware version
void insert_suffix_arena(uint32_t node_index, MutableNodeList *nodes,
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
  for (MachineLetter child_pos = 0; child_pos < node_num_children;
       child_pos++) {
    node = &nodes->nodes[node_index];
    const uint32_t child_index = get_child_index(node, child_pos);
    const MutableNode *child = &nodes->nodes[child_index];
    if (child->ml == ml) {
      insert_suffix_arena(child_index, nodes, word, pos + 1,
                          cached_node_indices);
      return;
    }
  }
  const int child_index = add_child_arena(node_index, nodes, ml);
  cached_node_indices[pos + 1] = child_index;
  insert_suffix_arena(child_index, nodes, word, pos + 1, cached_node_indices);
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
      NodeIndexList *children =
          (node->merged_into_index == NODE_NOT_MERGED)
              ? &node->children
              : &nodes->nodes[node->merged_into_index].children;
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
  // Use the fast compact-state builder when merging is enabled
  if (merging == KWG_MAKER_MERGE_EXACT) {
    return make_kwg_from_words_fast(words, output);
  }

  const bool output_dawg = (output == KWG_MAKER_OUTPUT_DAWG) ||
                           (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);
  const bool output_gaddag = (output == KWG_MAKER_OUTPUT_GADDAG) ||
                             (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);

  // Estimate sizes for arena allocation
  const int words_count = dictionary_word_list_get_count(words);
  const size_t estimated_nodes = (size_t)words_count * 12 + 100;
  const size_t arena_capacity = estimated_nodes;

  MutableNodeList *nodes =
      mutable_node_list_create_with_arena(estimated_nodes, arena_capacity);
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
    for (int i = 0; i < words_count; i++) {
      const DictionaryWord *word = dictionary_word_list_get_word(words, i);
      const int letters_in_common =
          get_letters_in_common(word, last_word, &last_word_length);
      const int start_index = cached_node_indices[letters_in_common];
      insert_suffix_arena(start_index, nodes, word, letters_in_common,
                          cached_node_indices);
    }
  }

  const int gaddag_root_node_index = mutable_node_list_add_root(nodes);

  if (output_gaddag) {
    last_word_length = 0;
    cached_node_indices[0] = gaddag_root_node_index;
    DictionaryWordList *gaddag_strings = dictionary_word_list_create();
    add_gaddag_strings(words, gaddag_strings);
    const int gaddag_count = dictionary_word_list_get_count(gaddag_strings);
    for (int gaddag_idx = 0; gaddag_idx < gaddag_count; gaddag_idx++) {
      const DictionaryWord *gaddag_string =
          dictionary_word_list_get_word(gaddag_strings, gaddag_idx);
      const int letters_in_common =
          get_letters_in_common(gaddag_string, last_word, &last_word_length);
      const int start_index = cached_node_indices[letters_in_common];
      insert_suffix_arena(start_index, nodes, gaddag_string, letters_in_common,
                          cached_node_indices);
    }
    dictionary_word_list_destroy(gaddag_strings);
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

// Optimized version for small dictionaries (endgame wordprune case).
// Uses the compact state builder for efficiency.
KWG *make_kwg_from_words_small(const DictionaryWordList *words,
                               kwg_maker_output_t output,
                               kwg_maker_merge_t merging) {
  // Use the fast compact-state builder when merging is enabled
  if (merging == KWG_MAKER_MERGE_EXACT) {
    return make_kwg_from_words_fast(words, output);
  }

  const bool output_dawg = (output == KWG_MAKER_OUTPUT_DAWG) ||
                           (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);
  const bool output_gaddag = (output == KWG_MAKER_OUTPUT_GADDAG) ||
                             (output == KWG_MAKER_OUTPUT_DAWG_AND_GADDAG);

  // Estimate sizes based on word count
  const int words_count = dictionary_word_list_get_count(words);
  const size_t estimated_gaddag_strings =
      output_gaddag ? (size_t)words_count * 7 : 0;
  const size_t estimated_nodes =
      (size_t)words_count * 12 + 100; // +100 for safety margin
  const size_t arena_capacity = estimated_nodes;

  // Create node list with arena allocator for child indices
  MutableNodeList *nodes =
      mutable_node_list_create_with_arena(estimated_nodes, arena_capacity);
  const int dawg_root_node_index = mutable_node_list_add_root(nodes);

  int cached_node_indices[MAX_KWG_STRING_LENGTH + 1];
  MachineLetter last_word[MAX_KWG_STRING_LENGTH];
  int last_word_length = 0;
  for (size_t i = 0; i < MAX_KWG_STRING_LENGTH; i++) {
    last_word[i] = 0;
  }

  if (output_dawg) {
    cached_node_indices[0] = dawg_root_node_index;
    for (int i = 0; i < words_count; i++) {
      const DictionaryWord *word = dictionary_word_list_get_word(words, i);
      const int letters_in_common =
          get_letters_in_common(word, last_word, &last_word_length);
      const int start_index = cached_node_indices[letters_in_common];
      insert_suffix_arena(start_index, nodes, word, letters_in_common,
                          cached_node_indices);
    }
  }

  const int gaddag_root_node_index = mutable_node_list_add_root(nodes);

  if (output_gaddag) {
    // Pre-allocate gaddag_strings with estimated capacity
    DictionaryWordList *gaddag_strings =
        dictionary_word_list_create_with_capacity(
            (int)estimated_gaddag_strings);
    add_gaddag_strings(words, gaddag_strings);

    last_word_length = 0;
    cached_node_indices[0] = gaddag_root_node_index;
    const int gaddag_count = dictionary_word_list_get_count(gaddag_strings);
    for (int gaddag_idx = 0; gaddag_idx < gaddag_count; gaddag_idx++) {
      const DictionaryWord *gaddag_string =
          dictionary_word_list_get_word(gaddag_strings, gaddag_idx);
      const int letters_in_common =
          get_letters_in_common(gaddag_string, last_word, &last_word_length);
      const int start_index = cached_node_indices[letters_in_common];
      insert_suffix_arena(start_index, nodes, gaddag_string, letters_in_common,
                          cached_node_indices);
    }
    dictionary_word_list_destroy(gaddag_strings);
  }

  MutableNode *dawg_root = &nodes->nodes[dawg_root_node_index];
  dawg_root->is_end = true;
  MutableNode *gaddag_root = &nodes->nodes[gaddag_root_node_index];
  gaddag_root->is_end = true;

  // Estimate final pointer list size (after merging, typically ~60-80% of node
  // count)
  const size_t estimated_final_nodes = nodes->count;
  NodePointerList *ordered_pointers =
      node_pointer_list_create_with_capacity(estimated_final_nodes);
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
