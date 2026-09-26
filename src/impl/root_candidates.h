#ifndef ROOT_CANDIDATES_H
#define ROOT_CANDIDATES_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/root_leaves.h"

// Generates the on-turn player's simulation candidates into move_list, the
// move_list's capacity best by static equity, sorted best first. With
// root_leaves NULL this is plain move generation. Otherwise every move of a
// larger pool (ROOT_CANDIDATES_POOL_CAPACITY, taken by the player's own
// static equity) gets the KLV3 contextual term for its leave and draw count,
// and the best by that equity are kept. Only the root changes: the player's
// KLV2, PAT and every cache are untouched.
void generate_root_candidates(const RootLeaves *root_leaves, Game *game,
                              MoveList *move_list);

// The contextual term root_leaves gives move in game: 0 for a move that draws
// nothing. Exposed for tests.
Equity root_candidates_get_move_adjustment(
    const Game *game,
    const Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS]
                            [MACHINE_LETTER_MAX_VALUE],
    const Move *move);

// The per-position tile adjustments for the on-turn player of game.
void root_candidates_compute_adjustments(
    const RootLeaves *root_leaves, const Game *game,
    Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE]);

#endif
