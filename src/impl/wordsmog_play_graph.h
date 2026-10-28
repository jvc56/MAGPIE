#ifndef WORDSMOG_PLAY_GRAPH_H
#define WORDSMOG_PLAY_GRAPH_H

#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include <stdbool.h>
#include <stdint.h>

// The tile placement plays of a wordsmog position with a WMP, enumerated
// lazily, best first: the full list of a wordsmog position's plays is far too
// large to make, above all with blanks.
//
// A play is a placement, a multiset of rack tiles that with the placement's
// board tiles is an anagram of a word, and an arrangement of those tiles on
// the placement's empty squares. The graph's nodes are the shadow's anchors,
// then the groups of an anchor (a placement and tile multiset each), then
// partial arrangements: a node pins one more square to a tile than its parent,
// in board order, and knows the exact equity of its best play. A blank stands
// for the designation with the best equity, then the lowest letter, so each
// arrangement is one play.
//
// The graph keeps its own generator, so it may outlive other move generation
// on the thread, but the game must not change while it exists.
typedef struct WordsmogPlayGraph WordsmogPlayGraph;
WordsmogPlayGraph *wordsmog_play_graph_create(const Game *game,
                                              move_sort_t move_sort_type);
// Writes the next play to move, in descending order of equity (or score,
// under score sort), then score. Returns false when every play has been
// written.
bool wordsmog_play_graph_next(WordsmogPlayGraph *graph, Move *move);
// The number of the position's plays: arrangements, with each blank
// designated as wordsmog_play_graph_next does, and plays with every
// designation. Expands every anchor but no arrangement.
void wordsmog_play_graph_count(WordsmogPlayGraph *graph, uint64_t *arrangements,
                               uint64_t *plays);
void wordsmog_play_graph_destroy(WordsmogPlayGraph *graph);

// Exploring the graph one node at a time. A node is a group (a placement and
// tile multiset) with tiles pinned to the first num_pinned of its num_squares
// empty squares, in board order: a natural letter, or WORDSMOG_PIN_BLANK for
// a blank whose designation is left to the play.
typedef struct WordsmogPlayNode {
  int group_idx;
  int num_squares;
  int num_pinned;
  MachineLetter pins[RACK_SIZE];
  // The equity and score of the node's best play.
  Equity equity;
  Equity score;
} WordsmogPlayNode;

// Expands every anchor and ranks the groups by their best plays, best first.
// Returns how many groups there are.
int wordsmog_play_graph_rank_groups(WordsmogPlayGraph *graph);
// The root of the group ranked rank (from 0) by
// wordsmog_play_graph_rank_groups, with no square pinned.
void wordsmog_play_graph_get_group(const WordsmogPlayGraph *graph, int rank,
                                   WordsmogPlayNode *node);
// The best play of node.
void wordsmog_play_graph_get_play(const WordsmogPlayGraph *graph,
                                  const WordsmogPlayNode *node, Move *move);
// How many arrangements node has, and plays with every designation.
void wordsmog_play_graph_count_node(const WordsmogPlayGraph *graph,
                                    const WordsmogPlayNode *node,
                                    uint64_t *arrangements, uint64_t *plays);
// The children of node: its next square pinned to each tile it has left that
// has a legal arrangement there, best first. Returns how many: none when every
// square is pinned.
int wordsmog_play_graph_get_children(const WordsmogPlayGraph *graph,
                                     const WordsmogPlayNode *node,
                                     WordsmogPlayNode children[RACK_SIZE + 1]);
// The board square that node's next pin goes on.
void wordsmog_play_graph_get_next_square(const WordsmogPlayGraph *graph,
                                         const WordsmogPlayNode *node, int *row,
                                         int *col);

#endif
