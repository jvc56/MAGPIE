#ifndef CENSUS_GEN_H
#define CENSUS_GEN_H

#include "../ent/game.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// Census leave generation (see census.h): each generation values every leave
// exactly on each of its boards, from one table of play scores a board, against
// the leaves of the generation before, and averages over the boards.
//
// The recipe is a comma-separated list of board counts, one a generation;
// KxN is K generations of N boards. 3x256,2048 is the recipe wolges measured
// best, and 4x256 a much faster one almost as good. Board b is the same board
// in every generation, so later generations reuse the play scores of earlier
// ones.
//
// Each board comes from a fresh game played greedily until the board holds a
// target number of tiles, cycling through the fill levels from two racks' worth
// of tiles to the most that still leaves a tile in the bag. A game that ends
// first, or overshoots into an empty bag, is replaced by another for the same
// target. The board's seed is derived from the run's seed and the board's
// number alone, so the result does not depend on the thread count.
typedef struct CensusGenArgs {
  // The game whose rules, lexicon and leaves the census uses. Not modified.
  const Game *game;
  const char *recipe;
  // The leaves are written to data/leaves as output_name, with each
  // generation's as output_name_gen_N.
  const char *output_name;
  const char *data_paths;
  uint64_t seed;
  int number_of_threads;
  // Play the boards greedily by score and start the first generation from
  // leaves of zero, as wolges's recommended census does. Otherwise the boards
  // are played by the game's leaves, and the first generation starts from
  // them.
  bool null_start;
  ThreadControl *thread_control;
} CensusGenArgs;

void census_gen(const CensusGenArgs *args, ErrorStack *error_stack);

#endif
