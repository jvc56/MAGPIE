#ifndef MOVE_GEN_H
#define MOVE_GEN_H

#include "../def/move_defs.h"
#include "../ent/best_move_carry.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/lane_shadow_cache.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "move_gen_base.h"
#include <stdbool.h>
#include <stdint.h>

void movegen_budget_init(MovegenBudget *budget, uint64_t limit);
uint64_t movegen_budget_get_spent(const MovegenBudget *budget);

typedef struct MoveGenArgs {
  const Game *game;
  move_record_t move_record_type;
  move_sort_t move_sort_type;
  const KWG *override_kwg;
  Equity eq_margin_movegen;

  // Inference passes the highest equity a move may have while the inferred
  // move stays within its margin: move generation stops at the first move
  // above it and skips anchors that cannot exceed it, so the move it returns
  // is above it exactly when the best move is. EQUITY_MAX_VALUE, for every
  // other caller, turns this off.
  Equity target_equity;

  MoveList *move_list;
  // Output: bitvector of machine letters that appear in any valid move.
  // Only used with MOVE_RECORD_TILES_PLAYED. Caller provides pointer; callee
  // writes result. Bit i set means machine letter i is playable.
  uint64_t *tiles_played_bv;
  // Input: initial set of known-playable tiles for MOVE_RECORD_TILES_PLAYED.
  // Movegen ORs further discoveries in. Default 0 (no known tiles).
  uint64_t initial_tiles_bv;
  // MOVE_RECORD_BEST_SMALL only: rank a play that uses the whole rack by its
  // score plus this bonus, e.g. twice the opponent's rack for going out in an
  // endgame. The recorded play keeps its real score. Default 0.
  Equity best_small_out_bonus;
  // MOVE_RECORD_BEST_SMALL only: if true, a play that ties the best value
  // replaces it when its tiny_move is larger, so the play returned does not
  // depend on the order in which anchors are searched (the endgame's
  // best-reply shortcuts find it in another order and must agree). Default
  // false: the first play found keeps a tie.
  bool best_small_canonical_ties;
  // Disables the PAT term even when the player has weights loaded.
  // Set by the inference paths, which build their own equity thresholds
  // from score plus leave and would misclassify moves whose recorded
  // equities carried the defense term.
  bool disable_pat;
  // Which loaded PAT classes to suppress (bits from pat_premium_class_t
  // and PAT_CLASS_MASK_WINDOWS; see PAT_CLASS_MASK_ALL). 0 applies every
  // class the weights have, so one weights file can serve both a fast
  // rollout mode and a full mode. Ignored when disable_pat is set.
  uint32_t pat_disabled_classes_mask;
  // In wordsmog with the PAT term, whether the WMP generator prices every play
  // that could be the best, as the alpha generator does, whatever the
  // player's limit (see player_get_wordsmog_pat_evals). Default false.
  bool wordsmog_exact_pat;
  // MOVE_RECORD_BEST by equity only: the best moves earlier generations
  // found for the same rack (see best_move_carry.h). Each one still legal
  // here starts the generation as its best move, lanes whose remembered
  // bound it beats are skipped (without PAT), and the generation's best move
  // is added, as a move and as the bound of the lanes searched. NULL (the
  // default) skips all of this.
  BestMoveCarry *best_move_carry;
  // MOVE_RECORD_BEST_SMALL only: lane shadows that earlier generations
  // recorded (see lane_shadow_cache.h). A lane the cache holds adds the
  // anchors recorded for it instead of being shadowed, and every other lane
  // is recorded. Not used with a word info table or in wordsmog, whose
  // shadows read more than the key holds. NULL (the default) shadows every
  // lane.
  LaneShadowCache *lane_shadow_cache;
} MoveGenArgs;

void gen_destroy_cache(void);

// The move generations made in this process so far, on every thread: each
// generate_moves call, and each generate_small_moves_in_lanes call (the
// endgame's incremental move list for a node, which stands in for a
// generate_moves call), whoever made it -- autoplay, a simulation, an
// inference, an endgame or pre-endgame solve. The difference between two
// readings is what was made in between.
//
// Each thread counts on its own MoveGen, unsynchronized, so nothing is shared
// in the hot path; this sums them, and so is only exact while no thread is
// generating moves: read it before and after a command, not during one.
//
// Only generations made while something accounts for them are counted: a
// movegen limit, a movegen budget on any thread, or a counting session. A
// caller that wants the count of some work starts a session around it.
uint64_t gen_get_movegen_count(void);
// A counting session: generations on every thread are counted from start to
// stop. Sessions nest. Started and stopped with no thread generating moves.
void gen_start_counting_movegens(void);
void gen_stop_counting_movegens(void);

// A command's movegen limit (-movegenlimit): a work budget that, unlike a
// time limit, is the same on any machine. Started before a command runs and
// stopped after it, on one thread, with no thread generating moves. While it
// runs, every generation on any thread counts against `limit`, and the one
// that reaches it stops the command as a user interrupt would, through
// `thread_control`. A command stops where it next checks for an interrupt --
// after a game, a simulation's iteration -- and generations are charged in
// batches, so it runs somewhat past the limit, never short of it: further
// for a player that generates many times a turn. A limit of 0 is none.
void gen_start_movegen_limit(uint64_t limit, ThreadControl *thread_control);
// Ends the running command's limit, and says whether it was reached.
bool gen_stop_movegen_limit(void);

// Makes `budget` the calling thread's move budget (NULL for none), charging
// the old one what this thread has not yet; returns the old one, for the
// caller to restore. A thread a search starts for a move takes the starting
// thread's budget (gen_get_movegen_budget) and clears it before it exits.
MovegenBudget *gen_set_movegen_budget(MovegenBudget *budget);
MovegenBudget *gen_get_movegen_budget(void);
// Whether the calling thread's move budget, if any, is spent: what a search
// checks wherever it checks its deadline.
bool gen_movegen_budget_spent(void);

// The most pool slots (see MAX_THREADS) live threads have held at once since
// the last gen_reset_slots_high_water, which starts the count again from the
// slots held now. A thread takes its slot on its first move generation and
// gives it back when it exits; past MAX_THREADS at once, magpie exits.
int gen_get_slots_high_water(void);
void gen_reset_slots_high_water(void);

// If override_kwg is NULL, the full KWG for the on-turn player is used,
// but if it is nonnull, override_kwg is used. The only use case for this
// so far is using a reduced wordlist kwg (done with wordprune) for endgame
// solving.
void generate_moves(const MoveGenArgs *args);

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

// The MOVE_RECORD_ALL_SMALL plays of the lanes whose bit is set in
// `lane_mask` (bit dir * BOARD_DIM + row_or_col: the BOARD_DIM horizontal
// rows, then the BOARD_DIM vertical columns), appended to args->move_list in
// generate_moves' lane order with no pass, skipping every anchor none of whose
// plays can span a square set in its lane's lane_hits entry (bit c: column c
// of a row, row c of a column). A play from an anchor starts right of the
// anchor's leftward limit (the previous anchor's square, or the square after
// it when that holds a tile) and covers, on either side of the anchor, no
// more empty squares than the rack has tiles and none where no rack tile
// fits, besides the tiles it plays through. lane_end[lane] receives the list
// count once that lane has been processed, so lane L's plays occupy
// [lane_end[L - 1], lane_end[L]) (from 0 for L == 0). For each lane in
// lane_mask, lane_anchors[lane] receives the bits of the lane's anchors and
// lane_generated[lane] those of the anchors not skipped. Serves the endgame's
// incremental move lists (path_move_lists.h).
void generate_small_moves_in_lanes(const MoveGenArgs *args, uint64_t lane_mask,
                                   const uint32_t *lane_hits, int *lane_end,
                                   uint32_t *lane_anchors,
                                   uint32_t *lane_generated);

// The MOVE_RECORD_TILES_PLAYED generation (see generate_moves) restricted as
// generate_small_moves_in_lanes restricts its own: to the lanes in `lane_mask`
// and, in each, to the anchors some of whose plays can span a square set in
// its lane_hits entry. Every play it finds is a play on the board, so the tile
// types it reports are playable; a tile type it does not report has no play
// from those anchors. Writes the tile types to *args->tiles_played_bv.
void generate_tiles_played_in_lanes(const MoveGenArgs *args, uint64_t lane_mask,
                                    const uint32_t *lane_hits);

// The MOVE_RECORD_BEST_SMALL generation restricted to the anchors of the lanes
// in `lane_mask` (indexed as for generate_small_moves_in_lanes), competing
// against `incumbent`: a play found elsewhere on the board, worth
// `incumbent_value` (its score, plus args->best_small_out_bonus when it uses
// the whole rack), or NULL for none. Anchors are pruned by their shadow bounds
// against the incumbent from the start, and with
// args->best_small_canonical_ties an equal-valued play wins only with a larger
// tiny_move, so the result is the argmax over the lanes and the incumbent under
// BEST_SMALL's ranking. Leaves the winner as the only entry of args->move_list
// and returns true, or returns false with the list empty when there is neither
// an incumbent nor a play in those lanes. Records no pass.
bool generate_best_small_move_in_lanes(const MoveGenArgs *args,
                                       uint64_t lane_mask,
                                       const SmallMove *incumbent,
                                       Equity incumbent_value);

MoveGen *get_movegen(void);

void gen_load_position(MoveGen *gen, const MoveGenArgs *args);

void gen_look_up_leaves_and_record_exchanges(MoveGen *gen);

void gen_shadow(MoveGen *gen);

#endif
