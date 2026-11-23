#ifndef INFERENCE_TEMPERATURE_FIT_H
#define INFERENCE_TEMPERATURE_FIT_H

#include "../ent/autoplay_results.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "autoplay.h"
#include <stdint.h>

// Fits inference temperatures by bag (see inference_temperatures.h) to
// self-play. Autoplay plays the games with the players as configured. At
// each of their tile placements made with tiles in the bag that keeps a
// leave, and at each exchange, the opponent infers the mover's leave from
// the play at each temperature of a grid, and the inference is scored by
// the log of the probability it gives the leave the mover really kept,
// mixed 1% with the draw prior as play mixes it, less the log of the draw
// prior's: its gain in nats over the prior.
//
// The placements fall into bands by the real bag before them: up to 7, 13,
// 20, 30, 45 and 60 tiles and above, the bands past 20 scaled by the
// distribution's starting bag over 86 (a 100-tile distribution's). A band's
// knot is its placements' mean bag, rounded, with the grid temperature of
// the highest mean gain; the exchanges, whatever the bag, give the
// exchange temperature. Inference for the fit uses one thread a play, so
// the table is the same for any thread count when the games are.

enum {
  INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS = 7,
};

typedef struct InferenceTemperatureFitArgs {
  // Filled as for the autoplay command, with the number of games; the fit
  // sets its move observer. Not otherwise modified.
  AutoplayArgs *autoplay_args;
  AutoplayResults *autoplay_results;
  // The table is written to data/strategy/<output_name>.csv.
  const char *output_name;
  const char *data_paths;
  // For the table's header.
  const char *lexicon_name;
  const char *leaves_name;
  const char *ld_name;
  const char *players_summary;
  int number_of_games;
  uint64_t seed;
  // The tiles in the bag once both racks are drawn.
  int starting_bag_size;
  ThreadControl *thread_control;
} InferenceTemperatureFitArgs;

// Upper edges of the bands for a distribution whose starting bag is
// starting_bag_size tiles; the last band has no upper edge.
void inference_temperature_fit_get_band_edges(
    int starting_bag_size,
    int edges[INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1]);

void inference_temperature_fit(const InferenceTemperatureFitArgs *args,
                               ErrorStack *error_stack);

#endif
