#ifndef ROOT_LEAVES_H
#define ROOT_LEAVES_H

#include "../def/letter_distribution_defs.h"
#include "../util/io_util.h"
#include "equity.h"
#include "rack.h"

// The contextual part of a KLV3 file (see PR #630's notes/KLV3.md), used only
// to rank a simulation's root candidates. A leave's KLV3 value is its KLV2
// value plus an additive term conditioned on the public unseen tiles (the bag
// plus the opponent's rack) and on how many tiles the move draws:
//
//   V(L, U, N, d) = KLV2(L) + sum_h L[h] * (bias[pool_bin(N), d, h]
//                                           + sum_u weight[d, h, u] U[u] / N)
//
// Move generation already scores every candidate with the player's KLV2, so
// adding the term to its equity gives the KLV3 equity exactly. Only the root
// uses this: rollouts keep the player's KLV2, and every rack-keyed cache
// (RIT, WMP leave maxima) stays valid. The KLV2 body in the file is skipped;
// the trailer must have been trained over the KLV2 the player uses.

enum {
  ROOT_LEAVES_DRAW_COUNT_HEADS = 8,
  ROOT_LEAVES_MAX_POOL_SIZE_BINS = 16,
};

typedef struct RootLeaves RootLeaves;

// Loads the trailer of data/lexica/<name>.klv3.
RootLeaves *root_leaves_create(const char *data_paths, const char *name,
                               ErrorStack *error_stack);
void root_leaves_destroy(RootLeaves *root_leaves);
const char *root_leaves_get_name(const RootLeaves *root_leaves);
// The number of tile types the model was trained on; it must equal the letter
// distribution's size.
int root_leaves_get_alphabet_size(const RootLeaves *root_leaves);

// Per-position adjustments: adjustments[d][h] is the term one held tile h adds
// to a leave when the move draws d tiles (draws of 7 or more use head 7, and
// d = 0 is always zero). unseen_counts is the public unseen multiset.
void root_leaves_compute_tile_adjustments(
    const RootLeaves *root_leaves, const int *unseen_counts, int unseen_total,
    Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE]);

// The contextual term for a leave that draws draw_count tiles.
Equity root_leaves_get_leave_adjustment(
    const Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS]
                            [MACHINE_LETTER_MAX_VALUE],
    const Rack *leave, int draw_count);

#endif
