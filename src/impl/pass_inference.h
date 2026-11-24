#ifndef PASS_INFERENCE_H
#define PASS_INFERENCE_H

#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/rack.h"
#include "../ent/rack_value_table.h"
#include "../util/io_util.h"
#include <stdint.h>

// What a pass says about the rack it was made with.
//
// A player who passes when they could have exchanged keeps all of their
// tiles: they judged the whole rack worth more than the best play or
// exchange they had. A rack value table (rack_value_table.h) holds that
// worth, on the scale of the leaves, so the passer is modeled as passing
// with probability
//
//   1 / (1 + exp((best - value - bonus) / temperature))
//
// where value is the table's value of their rack, best the static equity
// (score plus leave value) of the best play or exchange the rack has on the
// board, bonus what the passer is assumed to add to the value of passing,
// and temperature how sharply they choose, all in points.
//
// A pass inference is the exact posterior that rule gives over every rack the
// passer could hold: each rack of RACK_SIZE tiles that can be drawn from the
// tiles the observer has not seen, weighed by the number of ways to draw it
// (its prior) times the probability of passing with it (its mass).
//
// Like an inference of a play (inference.h), the passer's equities carry no
// pre-endgame adjustment and no PAT term. Every rack's best equity comes from
// one table of the board's census play scores (census_play_scores.h).

// The passer's assumed temperature and bonus when the caller has none.
#define PASS_INFERENCE_DEFAULT_TEMPERATURE 2.0
#define PASS_INFERENCE_DEFAULT_BONUS 0.0

typedef struct PassInferenceArgs {
  // The position the pass was made from: the passer is the player on turn.
  // The other player is the observer, and the tiles they have not seen are
  // the game's bag and whatever the game holds of the passer's rack.
  const Game *game;
  // The values of the passer's full racks. They must belong to the passer's
  // leaves (rack_value_table_get_klv_hash).
  const RackValueTable *rack_value_table;
  // In points; the temperature is above zero.
  double temperature;
  double bonus;
  int num_threads;
} PassInferenceArgs;

typedef struct PassInference PassInference;

// Infers the passer's rack. Returns NULL, with an error on the stack, for a
// position it cannot infer from:
// - the bag cannot fill both racks and still hold RACK_SIZE tiles
//   (game_get_real_bag_size), so the passer could not have exchanged: the
//   table's values, of racks with tiles to draw to, say little there;
// - the board is empty: the census play scores hold no opening placement
//   adjustment;
// - the game is not classic, or the players have different lexica;
// - the distribution has more letters than a rack value table holds;
// - the table is of another distribution or of other leaves;
// - the temperature is not above zero.
PassInference *pass_inference_create(const PassInferenceArgs *args,
                                     ErrorStack *error_stack);

void pass_inference_destroy(PassInference *pass_inference);

// The racks the passer could hold that the table values, numbered from 0 in
// the order of their letter counts (fewest blanks first, then fewest of the
// next letter, and so on).
uint32_t
pass_inference_get_number_of_racks(const PassInference *pass_inference);

// Sets rack to the rack_idx-th rack.
void pass_inference_get_rack(const PassInference *pass_inference,
                             uint32_t rack_idx, Rack *rack);

// The number of the rack, or UINT32_MAX if the passer could not hold it or
// the table does not value it.
uint32_t pass_inference_find_rack(const PassInference *pass_inference,
                                  const Rack *rack);

// The table's value of the rack and the equity of its best play or exchange.
Equity pass_inference_get_value(const PassInference *pass_inference,
                                uint32_t rack_idx);
Equity pass_inference_get_best_equity(const PassInference *pass_inference,
                                      uint32_t rack_idx);

// The number of ways to draw the rack from the unseen tiles.
double pass_inference_get_prior(const PassInference *pass_inference,
                                uint32_t rack_idx);

// The rack's prior times the probability of passing with it.
double pass_inference_get_mass(const PassInference *pass_inference,
                               uint32_t rack_idx);

// The sums over every rack. Their ratio is the probability that the passer,
// with a rack drawn at random, passes.
double pass_inference_get_total_prior(const PassInference *pass_inference);
double pass_inference_get_total_mass(const PassInference *pass_inference);

// The probability that the passer holds the rack, given the pass: its share
// of the total mass.
double pass_inference_get_probability(const PassInference *pass_inference,
                                      uint32_t rack_idx);

#endif
