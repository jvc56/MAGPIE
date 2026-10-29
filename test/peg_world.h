#ifndef PEG_WORLD_H
#define PEG_WORLD_H

// Hidden-information worlds for the PEG nets, from the mover's view only:
// public unseen counts, hypothetical deals, the static candidate pool, and the
// lanes a move touches.

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include <stdbool.h>
#include <stdint.h>

// Candidates: the static top PEG_STATIC_POOL_SIZE placements.
enum {
  PEG_STATIC_POOL_SIZE = 64,
};

// The side on turn's moves in static-equity order (into pool); the first
// PEG_STATIC_POOL_SIZE placements are copied to candidates. Returns their
// count; *pass_rank is the pass's rank among all moves (-1 = none).
int peg_world_static_pool(const Game *game, MoveList *pool, Move *candidates,
                          int *pass_rank);

// Construct a hypothetical world from PUBLIC unseen counts. This is only
// for feature aggregation and counterfactual targets, never to reveal the
// rollout's actual hidden tile to the move picker.
void peg_world_from_public_counts(const Game *root, const int *unseen, int tile,
                                  Game *scenario);

// Unseen counts from the mover's own view: the distribution less the board
// and the mover's own rack. Uses no hidden information.
int peg_world_public_unseen(const Game *game, int *unseen);

// Lanes whose plays can change when move lands on board (before the move):
// the lane-touch set of path_move_lists.c, duplicated for this harness.
uint64_t peg_world_lane_influence(const Board *board, const Move *move);

// The lane a move is played in: its row, or BOARD_DIM + its column when
// vertical (peg_world_lane_influence's bits).
int peg_world_move_lane(const Move *move);

// Highest-scoring play of the side on turn (WMP when available), optionally
// restricted to lanes and seeded with a known legal play. Writes the move.
void peg_world_best_scoring_in_lanes(const Game *game, MoveList *list,
                                     uint64_t lanes, const Move *seed,
                                     Move *best);

// Per hypothetical world, the opponent's best plays on the root board are
// kept for its PEG_WORLD_ROOT_LANES best lanes (Peg1RootContext,
// Peg2RootContext).
enum { PEG_WORLD_ROOT_LANES = 3 };

// Unseen tiles in a 2PEG: the opponent's rack and the two in the bag.
enum {
  PEG2_UNSEEN = RACK_SIZE + 2,
};

// Post-candidate 2PEG world: the bag held bag2[0..1]; the opponent's rack is
// the mover's unseen pool minus those two; the mover draws the first n_drawn
// of them (the candidate's tiles played, capped at two) and the bag keeps the
// rest. The opponent is then on turn.
void peg_world_after_candidate(const Game *root, const int *unseen,
                               const Move *cand, const MachineLetter *bag2,
                               int n_drawn, Game *world);

// ---- Wide 2PEG candidates ---------------------------------------------------
// A wide rank is a placement's index in the root's full move list sorted by
// equity, with the pass left out. compare_moves is a total order, so the
// first PEG_STATIC_POOL_SIZE wide ranks are exactly peg_world_static_pool's
// candidates and keep their ids. A list longer than
// PEG_WORLD_ALL_PLACEMENTS_CAP keeps the best by equity.
enum { PEG_WORLD_ALL_PLACEMENTS_CAP = 16384 };

int peg_world_all_placements(const Game *game, MoveList *wide, Move *moves);

// Resets base, a post-candidate world of peg_world_after_candidate, to another
// bag pair: only the racks, the bag, the end reason and the side on turn differ
// between a candidate's worlds. own_after is the mover's rack after the
// candidate, before drawing.
void peg_world_set_bag_pair(const int *unseen, const Rack *own_after,
                            const MachineLetter *bag2, int n_drawn, int mover,
                            int on_turn, Game *base);

// PEG_WORLD_NO_CACHE=1: rebuild every world from the root (the original
// path, kept for parity checks and timing).
bool peg_world_cache_enabled(void);

// root with its two bag tiles swapped for two other unseen tiles (taken from
// the opponent's rack): the mover's information is unchanged. False when the
// unseen pool has no two such tiles.
bool peg_world_swap_hidden_tiles(const Game *root, Game *swapped);

#endif
