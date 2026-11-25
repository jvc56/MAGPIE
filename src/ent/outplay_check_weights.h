#ifndef OUTPLAY_CHECK_WEIGHTS_H
#define OUTPLAY_CHECK_WEIGHTS_H

#include "../util/io_util.h"
#include "letter_distribution.h"
#include "rack.h"
#include <stdbool.h>

// The weights of the static player's out-play check (see outplay_check.h): a
// linear model of what the position after a bag-emptying play is worth to
// the mover, once the draw is known and the opponent is on turn. Every term
// comes in two kinds, by whether the opponent can then go out (tag "oo") or
// not ("no").
//
// The model's value, in points, of the mover's rack after the draw, given
// what the two sides could play from there (OutplayCheckFeatures):
//   - the static equity of keeping the rack in an empty-bag endgame: less
//     twice its face value and NON_OUTPLAY_CONSTANT_PENALTY;
//   - when the opponent can go out, less the best out play's score, plus
//     obo times that score; when it cannot, plus obs times its best score;
//   - const, plus n<k> for a rack of k tiles, plus face times its face value;
//   - for each letter on the rack, the letter's term, and for each pair of
//     letters on it (a letter with itself when it has two or more), the
//     pair's term;
//   - when the mover can go out next with the rack: own, plus ownb times the
//     best such play's score, plus ownl2 and ownl3 when such plays lie in at
//     least two and three lanes, plus ownface times the rack's face value,
//     plus ownn<k> for a rack of k tiles.
// center|<bag>|<leave> is taken off a candidate's value: the mean difference
// between the model's value and static equity for a bag of that size and a
// leave of that size, so that the check keeps static's effect of the leave's
// size on its own.
typedef struct OutplayCheckWeights OutplayCheckWeights;

// What the two sides could play after a candidate and a draw: the opponent,
// who is on turn, and the mover next.
typedef struct OutplayCheckFeatures {
  bool opponent_can_go_out;
  int opponent_best_out_score;
  // The opponent's best score when it cannot go out (the model reads it only
  // then).
  int opponent_best_score;
  bool own_can_go_out;
  int own_best_out_score;
  // The lanes (rows and columns) the mover's out plays lie in, counted up to 3
  // (the model reads whether there are 2, and 3).
  int own_out_lanes;
} OutplayCheckFeatures;

// Reads <data path>/strategy/<name>.csv. Each row is "term,value". The first
// row that is not blank or a comment (starting with '#') is
// "letterdistribution,<name>", which names the letter distribution the
// letters of the other rows are read with. The terms are those described
// above, written <term>|<tag> (a letter or pair term without a tag counts
// for both tags), and center|<bag>|<leave size>. A pair's letters are in the
// distribution's order. A term may appear more than once; its values add up.
OutplayCheckWeights *outplay_check_weights_create(const char *data_paths,
                                                  const char *name,
                                                  ErrorStack *error_stack);
void outplay_check_weights_destroy(OutplayCheckWeights *weights);

const char *outplay_check_weights_get_name(const OutplayCheckWeights *weights);
// The name of the letter distribution the weights were read with. They apply
// only to games with that distribution.
const char *
outplay_check_weights_get_ld_name(const OutplayCheckWeights *weights);

// The model's value, in points, of the mover holding `held` (see above).
double outplay_check_weights_value(const OutplayCheckWeights *weights,
                                   const LetterDistribution *ld,
                                   const Rack *held,
                                   const OutplayCheckFeatures *features);

// The center for a bag of bag_size tiles and a leave of leave_size tiles, in
// points; 0 where the file has none.
double outplay_check_weights_get_center(const OutplayCheckWeights *weights,
                                        int bag_size, int leave_size);

#endif
