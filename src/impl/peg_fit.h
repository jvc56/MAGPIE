#ifndef PEG_FIT_H
#define PEG_FIT_H

#include "../def/static_eval_defs.h"
#include "../ent/game.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// Fits the pre-endgame adjustments of static equity (a table in Quackle's
// format, indexed by the tiles left in the bag after a tile placement plus
// RACK_SIZE; see peg_adjust.h) to the outcomes of static play. The table
// fitted has number_of_values values, so it reaches the plays that leave up
// to number_of_values - RACK_SIZE - 1 tiles in the bag.
//
// Static self-play games give the positions: every turn with 1 to
// number_of_values - 1 tiles in the bag. A position's plays fall into
// classes, one for each table index a tile placement reaches, and a base class
// for the plays the table leaves alone (placements that leave the bag fuller,
// and exchanges). The best play of each class by static equity without the
// table is played out to the end of the game rollouts times, each rollout
// drawing the opponent's rack and the bag order from the tiles the mover
// cannot see, with the same draws for every class. Static play finishes the
// game, or, with endgame_plies set, plays until the bag is empty and the
// endgame is solved that many plies deep. A class's value in the position is
// its mean final spread change less its static equity. The fit is the least
// squares one of value = position term + class term, with the base class's
// term 0; its class terms are the new table.

enum {
  // A class for each index of the longest table, and the base class. A fit
  // of number_of_values values uses the first number_of_values + 1: class
  // number_of_values is its base class.
  PEG_FIT_MAX_NUMBER_OF_CLASSES = PEG_ADJUST_MAX_VALUES_LENGTH + 1,
};

typedef struct PegFitArgs {
  // The game whose rules, lexicon, leaves and pre-endgame adjustments
  // self-play uses; both players must have the same adjustments, if any.
  // Not modified.
  const Game *game;
  int number_of_games;
  int rollouts_per_class;
  // 0 plays endgames statically.
  int endgame_plies;
  // The length of the table to fit, from RACK_SIZE + 1 (the plays that empty
  // the bag) to PEG_ADJUST_MAX_VALUES_LENGTH.
  int number_of_values;
  // The endgame transposition table's share of memory.
  double tt_fraction_of_mem;
  // The table is written to data/strategy/<output_name>.csv.
  const char *output_name;
  const char *data_paths;
  uint64_t seed;
  int number_of_threads;
  ThreadControl *thread_control;
} PegFitArgs;

typedef struct PegFitPosition {
  // Where the position arose, so the fit can take positions in an order that
  // does not depend on the threads.
  int game_index;
  int turn;
  bool has_class[PEG_FIT_MAX_NUMBER_OF_CLASSES];
  // Mean final spread change less static equity, in points.
  double value[PEG_FIT_MAX_NUMBER_OF_CLASSES];
} PegFitPosition;

typedef struct PegFitResult {
  // In points; the base class's term is 0.
  double terms[PEG_FIT_MAX_NUMBER_OF_CLASSES];
  double standard_errors[PEG_FIT_MAX_NUMBER_OF_CLASSES];
  // The positions that have each class.
  int observations[PEG_FIT_MAX_NUMBER_OF_CLASSES];
} PegFitResult;

// The least squares fit of value = position term + class term over the
// positions, in the order given, for a table of number_of_values values:
// classes 0 to number_of_values - 1 are its indexes and class
// number_of_values is the base class.
void peg_fit_solve(const PegFitPosition *positions, int number_of_positions,
                   int number_of_values, PegFitResult *result);

// Self-plays args->number_of_games games, fits the table to their positions
// and writes it. With static endgames the table does not depend on the
// thread count.
void peg_fit(const PegFitArgs *args, ErrorStack *error_stack);

#endif
