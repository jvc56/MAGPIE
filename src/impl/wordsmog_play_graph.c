#include "wordsmog_play_graph.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../def/wordsmog_defs.h"
#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/wmp.h"
#include "../util/io_util.h"
#include "move_gen.h"
#include "move_gen_base.h"
#include "movegen_wordsmog.h"
#include "wmp_move_gen.h"
#include "wordsmog_arrangement.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The wordsmog play graph (see wordsmog_play_graph.h). Its frontier is the
// shadow's anchor heap and a heap of nodes:
// - An anchor is expanded into its groups, a group for each start column and
//   tile multiset with a legal arrangement, each pushed as the root of its
//   arrangements.
// - A node of a group is the arrangements that put its pins on its first
//   squares; expanding it pins one more square to each tile the node has
//   left. A node's equity is that of the best of its arrangements, exactly:
//   wordsmog_best_arrangement with its pins.
// - A node with every square pinned is one play.
// Every anchor's equity bounds its plays', so the next play is the best node
// popped while no anchor bounds more.

// A placement and tile multiset: what a group's plays share.
typedef struct WordsmogGraphGroup {
  int row;
  int dir;
  int start_col;
  int word_length;
  int tiles_played;
  // The play's tiles, with PLAYED_THROUGH_MARKER on the board's tiles; an
  // arrangement's tiles go at square_letter_idxs.
  MachineLetter strip[BOARD_DIM];
  int square_letter_idxs[RACK_SIZE];
  int num_squares;
  WordsmogSquare squares[RACK_SIZE];
  // The group's tiles, in the graph's tiles, shared by the start columns of
  // a subrack.
  int tiles_idx;
  // A play's equity is base_equity plus its arrangement's score and
  // adjustment, and its score is base_score plus its arrangement's score.
  Equity base_equity;
  Equity base_score;
  // The equity and score of its best play.
  Equity best_equity;
  Equity best_score;
} WordsmogGraphGroup;

// The arrangements of a group that put pins[square] on each square below
// num_pinned, with the equity and score of the best of them.
typedef struct WordsmogGraphNode {
  Equity equity;
  Equity score;
  // When the node was pushed: equal nodes come out first in, first out.
  uint32_t order;
  int group_idx;
  int num_pinned;
  MachineLetter pins[RACK_SIZE];
} WordsmogGraphNode;

struct WordsmogPlayGraph {
  // The graph's own generator, which keeps the position's anchors and lanes
  // between calls.
  MoveGen *gen;
  MoveList *move_list;
  WordsmogGraphGroup *groups;
  int num_groups;
  int groups_capacity;
  WordsmogTiles *tiles;
  int num_tiles;
  int tiles_capacity;
  // The groups best first, once wordsmog_play_graph_rank_groups has run.
  int *group_ranks;
  WordsmogGraphNode *heap;
  int heap_count;
  int heap_capacity;
  uint32_t next_order;
};

// Best equity, then score. Among nodes that tie, the one with more squares
// pinned, and then the smaller pins, so that a node's best play comes out
// before the plays it ties with, without visiting their nodes first.
static bool wordsmog_graph_node_ranks_above(const WordsmogGraphNode *a,
                                            const WordsmogGraphNode *b) {
  if (a->equity != b->equity) {
    return a->equity > b->equity;
  }
  if (a->score != b->score) {
    return a->score > b->score;
  }
  if (a->num_pinned != b->num_pinned) {
    return a->num_pinned > b->num_pinned;
  }
  const int pins_order = memcmp(a->pins, b->pins, sizeof(a->pins));
  if (pins_order != 0) {
    return pins_order < 0;
  }
  return a->order < b->order;
}

static void wordsmog_graph_push(WordsmogPlayGraph *graph,
                                WordsmogGraphNode node) {
  if (graph->heap_count == graph->heap_capacity) {
    graph->heap_capacity =
        graph->heap_capacity == 0 ? 256 : graph->heap_capacity * 2;
    graph->heap = realloc_or_die(graph->heap, (size_t)graph->heap_capacity *
                                                  sizeof(WordsmogGraphNode));
  }
  node.order = graph->next_order++;
  int child = graph->heap_count++;
  while (child > 0) {
    const int parent = (child - 1) / 2;
    if (!wordsmog_graph_node_ranks_above(&node, &graph->heap[parent])) {
      break;
    }
    graph->heap[child] = graph->heap[parent];
    child = parent;
  }
  graph->heap[child] = node;
}

static WordsmogGraphNode wordsmog_graph_pop(WordsmogPlayGraph *graph) {
  const WordsmogGraphNode top = graph->heap[0];
  const WordsmogGraphNode last = graph->heap[--graph->heap_count];
  int parent = 0;
  for (;;) {
    int child = 2 * parent + 1;
    if (child >= graph->heap_count) {
      break;
    }
    if (child + 1 < graph->heap_count &&
        wordsmog_graph_node_ranks_above(&graph->heap[child + 1],
                                        &graph->heap[child])) {
      child++;
    }
    if (!wordsmog_graph_node_ranks_above(&graph->heap[child], &last)) {
      break;
    }
    graph->heap[parent] = graph->heap[child];
    parent = child;
  }
  if (graph->heap_count > 0) {
    graph->heap[parent] = last;
  }
  return top;
}

// The node of group's arrangements with pins, and whether any is legal.
static bool wordsmog_graph_node_init(const WordsmogPlayGraph *graph,
                                     int group_idx, int num_pinned,
                                     const MachineLetter pins[RACK_SIZE],
                                     WordsmogGraphNode *node,
                                     WordsmogArrangement *arrangement) {
  const WordsmogGraphGroup *group = &graph->groups[group_idx];
  if (!wordsmog_best_arrangement(group->squares, group->num_squares,
                                 &graph->tiles[group->tiles_idx], pins,
                                 arrangement)) {
    return false;
  }
  node->equity =
      group->base_equity + arrangement->score + arrangement->adjustment;
  node->score = group->base_score + arrangement->score;
  node->group_idx = group_idx;
  node->num_pinned = num_pinned;
  memcpy(node->pins, pins, sizeof(node->pins));
  return true;
}

// Adds the groups of an anchor, each as the root of its arrangements.
static void wordsmog_graph_expand_anchor(WordsmogPlayGraph *graph,
                                         const Anchor *anchor) {
  MoveGen *gen = graph->gen;
  gen_load_anchor(gen, anchor);
  gen->max_tiles_to_play = anchor->tiles_to_play;
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  wmp_move_gen_set_playthrough_bit_rack(wgen, anchor, gen->row_squares, NULL,
                                        gen->wit_len_lane, NULL);
  wmp_move_gen_playthrough_metadata_init(wgen, anchor);
  wmp_move_gen_build_playthrough_subracks(wgen);
  const int num_subrack_combinations =
      wmp_move_gen_get_num_subrack_combinations(wgen);
  for (int subrack_idx = 0; subrack_idx < num_subrack_combinations;
       subrack_idx++) {
    BitRack main_word;
    const WMPEntry *entry =
        wmp_move_gen_get_subrack_entry(wgen, subrack_idx, false, &main_word);
    if (entry == NULL) {
      continue;
    }
    if (gen->wmp_prune_subracks_by_leave) {
      wgen->leave_value = wmp_move_gen_get_leave_value(wgen, subrack_idx);
    }
    if (gen->number_of_tiles_in_bag == 0) {
      wordmap_gen_set_empty_bag_leave(gen, anchor, subrack_idx);
    }
    if (graph->num_tiles == graph->tiles_capacity) {
      graph->tiles_capacity =
          graph->tiles_capacity == 0 ? 16 : graph->tiles_capacity * 2;
      graph->tiles = realloc_or_die(
          graph->tiles, (size_t)graph->tiles_capacity * sizeof(WordsmogTiles));
    }
    const int tiles_idx = graph->num_tiles;
    WordsmogTiles *tiles = &graph->tiles[tiles_idx];
    wordsmog_tiles_init(
        gen, wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx),
        &main_word, entry, &graph->tiles[tiles_idx]);
    bool tiles_used = false;
    for (int start_col = anchor->leftmost_start_col;
         start_col <= anchor->rightmost_start_col; start_col++) {
      if (graph->num_groups == graph->groups_capacity) {
        graph->groups_capacity =
            graph->groups_capacity == 0 ? 64 : graph->groups_capacity * 2;
        graph->groups =
            realloc_or_die(graph->groups, (size_t)graph->groups_capacity *
                                              sizeof(WordsmogGraphGroup));
      }
      WordsmogGraphGroup *group = &graph->groups[graph->num_groups];
      Equity base_score;
      group->num_squares =
          wordsmog_placement_squares(gen, start_col, group->squares,
                                     group->square_letter_idxs, &base_score);
      assert(group->num_squares == anchor->tiles_to_play);
      group->tiles_idx = tiles_idx;
      WordsmogArrangement best;
      if (!wordsmog_best_arrangement(group->squares, group->num_squares, tiles,
                                     NULL, &best)) {
        continue;
      }
      tiles_used = true;
      // Price the best arrangement as move generation would, and keep what
      // the group's other arrangements share with it.
      for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
        gen->playthrough_marked[group->square_letter_idxs[square_idx]] =
            best.tiles[square_idx];
      }
      const Equity score = wmp_play_score(gen, start_col);
      Move *move = move_list_get_spare_move(graph->move_list);
      set_play_for_record_wmp(gen, move, start_col, score);
      const Equity equity =
          get_move_equity_for_sort_type_wmp(gen, move, wgen->leave_value);
      group->row = gen->current_row_index;
      group->dir = gen->dir;
      group->start_col = start_col;
      group->word_length = wgen->word_length;
      group->tiles_played = anchor->tiles_to_play;
      memcpy(group->strip, gen->playthrough_marked, (size_t)wgen->word_length);
      group->base_equity = equity - best.score - best.adjustment;
      group->base_score = score - best.score;
      MachineLetter pins[RACK_SIZE];
      memset(pins, WORDSMOG_PIN_FREE, sizeof(pins));
      WordsmogGraphNode root;
      const bool legal = wordsmog_graph_node_init(graph, graph->num_groups, 0,
                                                  pins, &root, &best);
      assert(legal);
      (void)legal;
      group->best_equity = root.equity;
      group->best_score = root.score;
      graph->num_groups++;
      wordsmog_graph_push(graph, root);
    }
    if (tiles_used) {
      graph->num_tiles++;
    }
  }
}

// The children of node: one more square pinned to each tile the node has left
// that has a legal arrangement there. Returns how many.
static int wordsmog_graph_children(const WordsmogPlayGraph *graph,
                                   const WordsmogGraphNode *node,
                                   WordsmogGraphNode children[RACK_SIZE + 1]) {
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  const WordsmogTiles *tiles = &graph->tiles[group->tiles_idx];
  int counts[RACK_SIZE];
  int blanks = tiles->num_blanks;
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    counts[letter_idx] = tiles->counts[letter_idx];
  }
  for (int square_idx = 0; square_idx < node->num_pinned; square_idx++) {
    if (node->pins[square_idx] == WORDSMOG_PIN_BLANK) {
      blanks--;
      continue;
    }
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (tiles->letters[letter_idx] == node->pins[square_idx]) {
        counts[letter_idx]--;
      }
    }
  }
  MachineLetter pins[RACK_SIZE];
  memcpy(pins, node->pins, sizeof(pins));
  const int square_idx = node->num_pinned;
  const uint64_t cross_set = group->squares[square_idx].cross_set;
  WordsmogArrangement arrangement;
  int num_children = 0;
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    const MachineLetter ml = tiles->letters[letter_idx];
    if (counts[letter_idx] == 0 || ((cross_set >> ml) & 1U) == 0) {
      continue;
    }
    pins[square_idx] = ml;
    if (wordsmog_graph_node_init(graph, node->group_idx, square_idx + 1, pins,
                                 &children[num_children], &arrangement)) {
      num_children++;
    }
  }
  if (blanks > 0) {
    pins[square_idx] = WORDSMOG_PIN_BLANK;
    if (wordsmog_graph_node_init(graph, node->group_idx, square_idx + 1, pins,
                                 &children[num_children], &arrangement)) {
      num_children++;
    }
  }
  return num_children;
}

static void wordsmog_graph_expand_node(WordsmogPlayGraph *graph,
                                       const WordsmogGraphNode *node) {
  WordsmogGraphNode children[RACK_SIZE + 1];
  const int num_children = wordsmog_graph_children(graph, node, children);
  for (int child_idx = 0; child_idx < num_children; child_idx++) {
    wordsmog_graph_push(graph, children[child_idx]);
  }
}

// Writes the best play of node.
static void wordsmog_graph_write_play(const WordsmogPlayGraph *graph,
                                      const WordsmogGraphNode *node,
                                      Move *move) {
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  WordsmogArrangement arrangement;
  const bool legal = wordsmog_best_arrangement(
      group->squares, group->num_squares, &graph->tiles[group->tiles_idx],
      node->pins, &arrangement);
  assert(legal);
  (void)legal;
  MachineLetter strip[BOARD_DIM];
  memcpy(strip, group->strip, (size_t)group->word_length);
  for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
    strip[group->square_letter_idxs[square_idx]] =
        arrangement.tiles[square_idx];
  }
  move_set_all_except_equity(move, strip, 0, group->word_length - 1,
                             group->base_score + arrangement.score, group->row,
                             group->start_col, group->tiles_played, group->dir,
                             GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(group->dir)) {
    move_set_row_start(move, group->start_col);
    move_set_col_start(move, group->row);
  }
  move_set_equity(move, group->base_equity + arrangement.score +
                            arrangement.adjustment);
}

WordsmogPlayGraph *wordsmog_play_graph_create(const Game *game,
                                              move_sort_t move_sort_type) {
  WordsmogPlayGraph *graph = calloc_or_die(1, sizeof(WordsmogPlayGraph));
  graph->gen = calloc_or_die(1, sizeof(MoveGen));
  graph->move_list = move_list_create(1);
  // The graph ranks plays by their arrangements, without the PAT term, which
  // would rank the arrangements of a group differently (see
  // WordsmogPATGroup).
  const MoveGenArgs args = {.game = game,
                            .move_list = graph->move_list,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = move_sort_type,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .disable_pat = true};
  MoveGen *gen = graph->gen;
  gen_load_position(gen, &args);
  if (!gen->is_wordsmog || !wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    log_fatal("the wordsmog play graph needs a wordsmog game with a WMP");
  }
  // As generate_moves: the leaves, the rack's subracks with their WMP
  // entries, and the shadow's anchors. The exchanges recorded on the way are
  // not part of the graph.
  gen_look_up_leaves_and_record_exchanges(gen);
  const bool check_leaves =
      gen->number_of_tiles_in_bag > 0 && gen->move_sort_type != MOVE_SORT_SCORE;
  wmp_move_gen_check_nonplaythrough_existence(&gen->wmp_move_gen, check_leaves,
                                              &gen->leave_map, false, false);
  gen_shadow(gen);
  gen->tiles_played = 0;
  // Values outside their valid ranges, so the first anchor loads its lane.
  gen->current_row_index = -1;
  gen->dir = -1;
  return graph;
}

bool wordsmog_play_graph_next(WordsmogPlayGraph *graph, Move *move) {
  AnchorHeap *anchors = &graph->gen->anchor_heap;
  for (;;) {
    if (anchors->count > 0 && (graph->heap_count == 0 ||
                               anchors->anchors[0].highest_possible_equity >=
                                   graph->heap[0].equity)) {
      const Anchor anchor = anchor_heap_extract_max(anchors);
      wordsmog_graph_expand_anchor(graph, &anchor);
      continue;
    }
    if (graph->heap_count == 0) {
      return false;
    }
    const WordsmogGraphNode node = wordsmog_graph_pop(graph);
    if (node.num_pinned == graph->groups[node.group_idx].num_squares) {
      wordsmog_graph_write_play(graph, &node, move);
      return true;
    }
    wordsmog_graph_expand_node(graph, &node);
  }
}

void wordsmog_play_graph_count(WordsmogPlayGraph *graph, uint64_t *arrangements,
                               uint64_t *plays) {
  AnchorHeap *anchors = &graph->gen->anchor_heap;
  while (anchors->count > 0) {
    const Anchor anchor = anchor_heap_extract_max(anchors);
    wordsmog_graph_expand_anchor(graph, &anchor);
  }
  *arrangements = 0;
  *plays = 0;
  for (int group_idx = 0; group_idx < graph->num_groups; group_idx++) {
    const WordsmogGraphGroup *group = &graph->groups[group_idx];
    uint64_t group_arrangements;
    uint64_t group_plays;
    wordsmog_count_arrangements(group->squares, group->num_squares,
                                &graph->tiles[group->tiles_idx], NULL,
                                &group_arrangements, &group_plays);
    *arrangements += group_arrangements;
    *plays += group_plays;
  }
}

// Exploring the graph's nodes (see wordsmog_play_graph.h).

static void wordsmog_graph_node_to_public(const WordsmogPlayGraph *graph,
                                          const WordsmogGraphNode *node,
                                          WordsmogPlayNode *public_node) {
  public_node->group_idx = node->group_idx;
  public_node->num_squares = graph->groups[node->group_idx].num_squares;
  public_node->num_pinned = node->num_pinned;
  memcpy(public_node->pins, node->pins, sizeof(public_node->pins));
  public_node->equity = node->equity;
  public_node->score = node->score;
}

static void wordsmog_graph_node_from_public(const WordsmogPlayNode *public_node,
                                            WordsmogGraphNode *node) {
  node->equity = public_node->equity;
  node->score = public_node->score;
  node->order = 0;
  node->group_idx = public_node->group_idx;
  node->num_pinned = public_node->num_pinned;
  memcpy(node->pins, public_node->pins, sizeof(node->pins));
}

// A group's rank key: its best play's equity and score, then its index.
typedef struct WordsmogGroupRank {
  Equity equity;
  Equity score;
  int group_idx;
} WordsmogGroupRank;

static int compare_wordsmog_group_ranks(const void *a, const void *b) {
  const WordsmogGroupRank *rank_a = a;
  const WordsmogGroupRank *rank_b = b;
  if (rank_a->equity != rank_b->equity) {
    return rank_a->equity > rank_b->equity ? -1 : 1;
  }
  if (rank_a->score != rank_b->score) {
    return rank_a->score > rank_b->score ? -1 : 1;
  }
  return rank_a->group_idx - rank_b->group_idx;
}

int wordsmog_play_graph_rank_groups(WordsmogPlayGraph *graph) {
  AnchorHeap *anchors = &graph->gen->anchor_heap;
  while (anchors->count > 0) {
    const Anchor anchor = anchor_heap_extract_max(anchors);
    wordsmog_graph_expand_anchor(graph, &anchor);
  }
  WordsmogGroupRank *ranks = malloc_or_die((size_t)(graph->num_groups + 1) *
                                           sizeof(WordsmogGroupRank));
  for (int group_idx = 0; group_idx < graph->num_groups; group_idx++) {
    ranks[group_idx].equity = graph->groups[group_idx].best_equity;
    ranks[group_idx].score = graph->groups[group_idx].best_score;
    ranks[group_idx].group_idx = group_idx;
  }
  qsort(ranks, (size_t)graph->num_groups, sizeof(WordsmogGroupRank),
        compare_wordsmog_group_ranks);
  free(graph->group_ranks);
  graph->group_ranks =
      malloc_or_die((size_t)(graph->num_groups + 1) * sizeof(int));
  for (int rank = 0; rank < graph->num_groups; rank++) {
    graph->group_ranks[rank] = ranks[rank].group_idx;
  }
  free(ranks);
  return graph->num_groups;
}

void wordsmog_play_graph_get_group(const WordsmogPlayGraph *graph, int rank,
                                   WordsmogPlayNode *node) {
  assert(graph->group_ranks != NULL);
  assert(rank >= 0 && rank < graph->num_groups);
  const int group_idx = graph->group_ranks[rank];
  const WordsmogGraphGroup *group = &graph->groups[group_idx];
  node->group_idx = group_idx;
  node->num_squares = group->num_squares;
  node->num_pinned = 0;
  memset(node->pins, WORDSMOG_PIN_FREE, sizeof(node->pins));
  node->equity = group->best_equity;
  node->score = group->best_score;
}

void wordsmog_play_graph_get_play(const WordsmogPlayGraph *graph,
                                  const WordsmogPlayNode *node, Move *move) {
  WordsmogGraphNode internal_node;
  wordsmog_graph_node_from_public(node, &internal_node);
  wordsmog_graph_write_play(graph, &internal_node, move);
}

void wordsmog_play_graph_count_node(const WordsmogPlayGraph *graph,
                                    const WordsmogPlayNode *node,
                                    uint64_t *arrangements, uint64_t *plays) {
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  wordsmog_count_arrangements(group->squares, group->num_squares,
                              &graph->tiles[group->tiles_idx], node->pins,
                              arrangements, plays);
}

int wordsmog_play_graph_get_children(const WordsmogPlayGraph *graph,
                                     const WordsmogPlayNode *node,
                                     WordsmogPlayNode children[RACK_SIZE + 1]) {
  if (node->num_pinned == node->num_squares) {
    return 0;
  }
  WordsmogGraphNode internal_node;
  wordsmog_graph_node_from_public(node, &internal_node);
  WordsmogGraphNode internal_children[RACK_SIZE + 1];
  const int num_children =
      wordsmog_graph_children(graph, &internal_node, internal_children);
  // Best first, as the graph would pop them.
  for (int child_idx = 1; child_idx < num_children; child_idx++) {
    const WordsmogGraphNode child = internal_children[child_idx];
    int insert_idx = child_idx;
    while (insert_idx > 0 && wordsmog_graph_node_ranks_above(
                                 &child, &internal_children[insert_idx - 1])) {
      internal_children[insert_idx] = internal_children[insert_idx - 1];
      insert_idx--;
    }
    internal_children[insert_idx] = child;
  }
  for (int child_idx = 0; child_idx < num_children; child_idx++) {
    wordsmog_graph_node_to_public(graph, &internal_children[child_idx],
                                  &children[child_idx]);
  }
  return num_children;
}

void wordsmog_play_graph_get_next_square(const WordsmogPlayGraph *graph,
                                         const WordsmogPlayNode *node, int *row,
                                         int *col) {
  assert(node->num_pinned < node->num_squares);
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  const int lane_position =
      group->start_col + group->square_letter_idxs[node->num_pinned];
  if (board_is_dir_vertical(group->dir)) {
    *row = lane_position;
    *col = group->row;
  } else {
    *row = group->row;
    *col = lane_position;
  }
}

void wordsmog_play_graph_destroy(WordsmogPlayGraph *graph) {
  if (graph == NULL) {
    return;
  }
  free(graph->heap);
  free(graph->groups);
  free(graph->tiles);
  free(graph->group_ranks);
  move_list_destroy(graph->move_list);
  generator_destroy(graph->gen);
  free(graph);
}
