#include "inference_temperature_fit.h"

#include "../compat/cpthread.h"
#include "../def/game_history_defs.h"
#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/alias_method.h"
#include "../ent/autoplay_results.h"
#include "../ent/data_filepaths.h"
#include "../ent/encoded_rack.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "autoplay.h"
#include "inference.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  // The temperatures tried, and the draw prior (a temperature so high it
  // weighs every leave by its draws alone) last.
  INFERENCE_TEMPERATURE_FIT_GRID_SIZE = 14,
  INFERENCE_TEMPERATURE_FIT_PRIOR_INDEX = INFERENCE_TEMPERATURE_FIT_GRID_SIZE,
  INFERENCE_TEMPERATURE_FIT_NUMBER_OF_TEMPERATURES =
      INFERENCE_TEMPERATURE_FIT_GRID_SIZE + 1,
  // The grid index of temperature 8, the constant default, for comparison.
  INFERENCE_TEMPERATURE_FIT_T8_INDEX = 9,
  INFERENCE_TEMPERATURE_FIT_INITIAL_CAPACITY = 1024,
  // Points: every leave is admitted, as in softmax inference.
  INFERENCE_TEMPERATURE_FIT_MARGIN = 10000,
  // The 100-tile distribution's starting bag, which the default band edges
  // are for.
  INFERENCE_TEMPERATURE_FIT_STANDARD_BAG = 86,
  // Band edges up to this many tiles are the same for every distribution.
  INFERENCE_TEMPERATURE_FIT_FIXED_EDGE_MAX = 20,
};

static const double inference_temperature_fit_grid
    [INFERENCE_TEMPERATURE_FIT_NUMBER_OF_TEMPERATURES] = {
        0.5, 1, 1.5, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32, 100000};

static const int inference_temperature_fit_standard_edges
    [INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1] = {7, 13, 20, 30, 45, 60};

// The share of the draw prior mixed into a posterior, as play mixes it.
#define INFERENCE_TEMPERATURE_FIT_PRIOR_MIX 0.01

typedef struct FitObservation {
  uint64_t game_number;
  int pair_game_number;
  int turn_number;
  int bag;
  bool is_exchange;
  // Nats over the draw prior at each grid temperature.
  double gain[INFERENCE_TEMPERATURE_FIT_GRID_SIZE];
} FitObservation;

typedef struct FitWorker {
  InferenceCtx *inference_ctx;
  InferenceResults *inference_results;
  Rack played;
  Rack known;
  Rack observer;
  FitObservation *observations;
  int number_of_observations;
  int capacity;
} FitWorker;

typedef struct FitShared {
  const InferenceTemperatureFitArgs *args;
  FitWorker *workers;
  int number_of_workers;
  cpthread_mutex_t mutex;
  int failed_inferences;
} FitShared;

void inference_temperature_fit_get_band_edges(
    int starting_bag_size,
    int edges[INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1]) {
  for (int band_idx = 0;
       band_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1; band_idx++) {
    const int standard_edge =
        inference_temperature_fit_standard_edges[band_idx];
    if (standard_edge <= INFERENCE_TEMPERATURE_FIT_FIXED_EDGE_MAX) {
      edges[band_idx] = standard_edge;
    } else {
      edges[band_idx] = (int)lround((double)standard_edge * starting_bag_size /
                                    INFERENCE_TEMPERATURE_FIT_STANDARD_BAG);
    }
    // Strictly ascending, whatever the scale.
    if (band_idx > 0 && edges[band_idx] <= edges[band_idx - 1]) {
      edges[band_idx] = edges[band_idx - 1] + 1;
    }
  }
}

static int fit_get_band(const int *edges, int bag) {
  for (int band_idx = 0;
       band_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1; band_idx++) {
    if (bag <= edges[band_idx]) {
      return band_idx;
    }
  }
  return INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1;
}

// The posterior probability of truth, read from the alias table the
// inference sampled from.
static double fit_get_probability(const InferenceResults *results,
                                  const Rack *truth) {
  const AliasMethod *alias_method = inference_results_get_alias_method(results);
  if (alias_method->num_items == 0) {
    return 0;
  }
  Rack rack;
  rack_set_dist_size_and_reset(&rack, rack_get_dist_size(truth));
  double total = 0;
  for (uint32_t item_idx = 0; item_idx < alias_method->num_items; item_idx++) {
    const AliasMethodItem *item = &alias_method->items[item_idx];
    rack_decode(&item->rack, &rack);
    if (racks_are_equal(&rack, truth)) {
      total += item->probability;
    }
    rack_decode(&alias_method->items[item->alias].rack, &rack);
    if (racks_are_equal(&rack, truth)) {
      total += 1 - item->probability;
    }
  }
  return total / alias_method->num_items;
}

static void fit_add_observation(FitWorker *worker,
                                const FitObservation *observation) {
  if (worker->number_of_observations == worker->capacity) {
    worker->capacity *= 2;
    worker->observations =
        realloc_or_die(worker->observations,
                       sizeof(FitObservation) * (size_t)worker->capacity);
  }
  worker->observations[worker->number_of_observations++] = *observation;
}

// Sets played to the tiles a tile placement takes from the rack.
static void fit_set_played_tiles(const Move *move, Rack *played) {
  rack_reset(played);
  const int tiles_length = move_get_tiles_length(move);
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    rack_add_letter(played, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
  }
}

static void fit_count_failure(FitShared *shared) {
  cpthread_mutex_lock(&shared->mutex);
  shared->failed_inferences++;
  cpthread_mutex_unlock(&shared->mutex);
}

static void fit_observe_move(void *data, int worker_index, const Game *game,
                             const Move *move, const Rack *leave,
                             uint64_t game_number, int pair_game_number,
                             int turn_number) {
  FitShared *shared = (FitShared *)data;
  const game_event_t type = move_get_type(move);
  const bool is_exchange = type == GAME_EVENT_EXCHANGE;
  if (type != GAME_EVENT_TILE_PLACEMENT_MOVE && !is_exchange) {
    return;
  }
  const int bag = game_get_real_bag_size(game);
  if (bag == 0 || rack_is_empty(leave)) {
    return;
  }
  FitWorker *worker = &shared->workers[worker_index];
  const int mover = game_get_player_on_turn_index(game);
  if (is_exchange) {
    rack_reset(&worker->played);
  } else {
    fit_set_played_tiles(move, &worker->played);
  }
  rack_reset(&worker->known);
  rack_copy(&worker->observer,
            player_get_rack(game_get_player(game, 1 - mover)));
  double probabilities[INFERENCE_TEMPERATURE_FIT_NUMBER_OF_TEMPERATURES];
  ErrorStack *error_stack = error_stack_create();
  for (int temperature_idx = 0;
       temperature_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_TEMPERATURES;
       temperature_idx++) {
    InferenceArgs inference_args;
    infer_args_fill(&inference_args, 0,
                    int_to_equity(INFERENCE_TEMPERATURE_FIT_MARGIN), NULL, game,
                    1, worker_index, 0, shared->args->thread_control, false,
                    true, mover, move_get_score(move),
                    is_exchange ? move_get_tiles_played(move) : 0,
                    &worker->played, &worker->known, &worker->observer);
    inference_args.temperature =
        double_to_equity(inference_temperature_fit_grid[temperature_idx]);
    if (!is_exchange) {
      inference_args.target_move = move;
    }
    infer(&inference_args, &worker->inference_ctx, worker->inference_results,
          error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_destroy(error_stack);
      fit_count_failure(shared);
      return;
    }
    probabilities[temperature_idx] =
        fit_get_probability(worker->inference_results, leave);
  }
  error_stack_destroy(error_stack);
  const double prior = probabilities[INFERENCE_TEMPERATURE_FIT_PRIOR_INDEX];
  if (prior <= 0) {
    fit_count_failure(shared);
    return;
  }
  FitObservation observation = {
      .game_number = game_number,
      .pair_game_number = pair_game_number,
      .turn_number = turn_number,
      .bag = bag,
      .is_exchange = is_exchange,
  };
  for (int temperature_idx = 0;
       temperature_idx < INFERENCE_TEMPERATURE_FIT_GRID_SIZE;
       temperature_idx++) {
    const double mixed = ((1 - INFERENCE_TEMPERATURE_FIT_PRIOR_MIX) *
                          probabilities[temperature_idx]) +
                         (INFERENCE_TEMPERATURE_FIT_PRIOR_MIX * prior);
    observation.gain[temperature_idx] = log(mixed) - log(prior);
  }
  fit_add_observation(worker, &observation);
}

static int fit_compare_observations(const void *first, const void *second) {
  const FitObservation *first_observation = (const FitObservation *)first;
  const FitObservation *second_observation = (const FitObservation *)second;
  if (first_observation->game_number != second_observation->game_number) {
    return first_observation->game_number < second_observation->game_number ? -1
                                                                            : 1;
  }
  if (first_observation->pair_game_number !=
      second_observation->pair_game_number) {
    return first_observation->pair_game_number <
                   second_observation->pair_game_number
               ? -1
               : 1;
  }
  if (first_observation->turn_number != second_observation->turn_number) {
    return first_observation->turn_number < second_observation->turn_number ? -1
                                                                            : 1;
  }
  return 0;
}

// The sums over one band's plays, or over the exchanges.
typedef struct FitBand {
  int count;
  double bag_total;
  double gain_total[INFERENCE_TEMPERATURE_FIT_GRID_SIZE];
} FitBand;

static void fit_band_add(FitBand *band, const FitObservation *observation) {
  band->count++;
  band->bag_total += observation->bag;
  for (int temperature_idx = 0;
       temperature_idx < INFERENCE_TEMPERATURE_FIT_GRID_SIZE;
       temperature_idx++) {
    band->gain_total[temperature_idx] += observation->gain[temperature_idx];
  }
}

// The grid index of the band's highest mean gain, the lower temperature on a
// tie.
static int fit_band_best_index(const FitBand *band) {
  int best_idx = 0;
  for (int temperature_idx = 1;
       temperature_idx < INFERENCE_TEMPERATURE_FIT_GRID_SIZE;
       temperature_idx++) {
    if (band->gain_total[temperature_idx] > band->gain_total[best_idx]) {
      best_idx = temperature_idx;
    }
  }
  return best_idx;
}

static void fit_add_band_comment(StringBuilder *sb, const char *label,
                                 const FitBand *band) {
  const int best_idx = fit_band_best_index(band);
  string_builder_add_formatted_string(
      sb,
      "# %s: %d plays, mean bag %.1f, %+.3f nats over the draw prior at T "
      "%g, %+.3f at T 8\n",
      label, band->count, band->bag_total / band->count,
      band->gain_total[best_idx] / band->count,
      inference_temperature_fit_grid[best_idx],
      band->gain_total[INFERENCE_TEMPERATURE_FIT_T8_INDEX] / band->count);
}

// Writes the table (rows, preceded by a header of comments) to sb.
static void fit_build_table(const InferenceTemperatureFitArgs *args,
                            const FitBand *bands, const FitBand *exchanges,
                            const int *edges, int failed_inferences,
                            StringBuilder *sb) {
  string_builder_add_formatted_string(
      sb,
      "# Inference temperatures fitted by inftempfit: lexicon %s, leaves %s, "
      "letter distribution %s, %d games, seed %llu.\n"
      "# Players: %s.\n"
      "# Rows are bag,temperature (points), the bag the real one before the "
      "observed play, and exchange,temperature.\n",
      args->lexicon_name, args->leaves_name, args->ld_name,
      args->number_of_games, (unsigned long long)args->seed,
      args->players_summary);
  if (failed_inferences > 0) {
    string_builder_add_formatted_string(
        sb, "# %d plays left out: their inference failed.\n",
        failed_inferences);
  }
  for (int band_idx = 0; band_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS;
       band_idx++) {
    if (bands[band_idx].count == 0) {
      continue;
    }
    const int low = band_idx == 0 ? 1 : edges[band_idx - 1] + 1;
    char *label = band_idx == INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1
                      ? get_formatted_string("bag %d+", low)
                      : get_formatted_string("bag %d-%d", low, edges[band_idx]);
    fit_add_band_comment(sb, label, &bands[band_idx]);
    free(label);
  }
  if (exchanges->count > 0) {
    fit_add_band_comment(sb, "exchanges", exchanges);
  }
  for (int band_idx = 0; band_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS;
       band_idx++) {
    const FitBand *band = &bands[band_idx];
    if (band->count == 0) {
      continue;
    }
    string_builder_add_formatted_string(
        sb, "%ld,%g\n", lround(band->bag_total / band->count),
        inference_temperature_fit_grid[fit_band_best_index(band)]);
  }
  if (exchanges->count > 0) {
    string_builder_add_formatted_string(
        sb, "exchange,%g\n",
        inference_temperature_fit_grid[fit_band_best_index(exchanges)]);
  }
}

static void fit_write_table(const InferenceTemperatureFitArgs *args,
                            const char *table, ErrorStack *error_stack) {
  char *filename = data_filepaths_get_writable_filename(
      args->data_paths, args->output_name,
      DATA_FILEPATH_TYPE_INFERENCE_TEMPERATURES, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return;
  }
  write_string_to_file(filename, "w", table, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print_formatted(args->thread_control,
                                   "inftempfit: wrote %s\n", filename);
  }
  free(filename);
}

// Fits the table to the observations gathered by the workers and writes it.
static void fit_finish(const FitShared *shared, ErrorStack *error_stack) {
  const InferenceTemperatureFitArgs *args = shared->args;
  int number_of_observations = 0;
  for (int worker_idx = 0; worker_idx < shared->number_of_workers;
       worker_idx++) {
    number_of_observations +=
        shared->workers[worker_idx].number_of_observations;
  }
  if (number_of_observations == 0) {
    error_stack_push(error_stack,
                     ERROR_STATUS_INFERENCE_TEMPERATURE_FIT_NO_PLAYS,
                     string_duplicate("inftempfit: self-play gave no plays to "
                                      "fit inference temperatures to"));
    return;
  }
  // In the order the games were played, whatever thread played them, so that
  // the sums are the same for any thread count.
  FitObservation *observations =
      malloc_or_die(sizeof(FitObservation) * (size_t)number_of_observations);
  int observation_idx = 0;
  for (int worker_idx = 0; worker_idx < shared->number_of_workers;
       worker_idx++) {
    const FitWorker *worker = &shared->workers[worker_idx];
    for (int idx = 0; idx < worker->number_of_observations; idx++) {
      observations[observation_idx++] = worker->observations[idx];
    }
  }
  qsort(observations, (size_t)number_of_observations, sizeof(FitObservation),
        fit_compare_observations);
  int edges[INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS - 1];
  inference_temperature_fit_get_band_edges(args->starting_bag_size, edges);
  FitBand bands[INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS] = {{0}};
  FitBand exchanges = {0};
  for (int idx = 0; idx < number_of_observations; idx++) {
    const FitObservation *observation = &observations[idx];
    if (observation->is_exchange) {
      fit_band_add(&exchanges, observation);
    } else {
      fit_band_add(&bands[fit_get_band(edges, observation->bag)], observation);
    }
  }
  free(observations);
  StringBuilder *sb = string_builder_create();
  fit_build_table(args, bands, &exchanges, edges, shared->failed_inferences,
                  sb);
  thread_control_print_formatted(args->thread_control, "%s",
                                 string_builder_peek(sb));
  // A table needs a bag row: a fit of exchanges alone is not one.
  bool has_bag_row = false;
  for (int band_idx = 0; band_idx < INFERENCE_TEMPERATURE_FIT_NUMBER_OF_BANDS;
       band_idx++) {
    has_bag_row = has_bag_row || bands[band_idx].count > 0;
  }
  if (has_bag_row) {
    fit_write_table(args, string_builder_peek(sb), error_stack);
  } else {
    error_stack_push(error_stack,
                     ERROR_STATUS_INFERENCE_TEMPERATURE_FIT_NO_PLAYS,
                     string_duplicate("inftempfit: self-play gave no tile "
                                      "placements to fit inference "
                                      "temperatures to"));
  }
  string_builder_destroy(sb);
}

void inference_temperature_fit(const InferenceTemperatureFitArgs *args,
                               ErrorStack *error_stack) {
  AutoplayArgs *autoplay_args = args->autoplay_args;
  FitShared shared = {
      .args = args,
      .number_of_workers = autoplay_args->num_threads,
      .failed_inferences = 0,
  };
  cpthread_mutex_init(&shared.mutex);
  shared.workers =
      malloc_or_die(sizeof(FitWorker) * (size_t)shared.number_of_workers);
  const int ld_size = ld_get_size(autoplay_args->game_args->ld);
  for (int worker_idx = 0; worker_idx < shared.number_of_workers;
       worker_idx++) {
    FitWorker *worker = &shared.workers[worker_idx];
    worker->inference_ctx = NULL;
    worker->inference_results = inference_results_create(NULL);
    rack_set_dist_size_and_reset(&worker->played, ld_size);
    rack_set_dist_size_and_reset(&worker->known, ld_size);
    rack_set_dist_size_and_reset(&worker->observer, ld_size);
    worker->capacity = INFERENCE_TEMPERATURE_FIT_INITIAL_CAPACITY;
    worker->number_of_observations = 0;
    worker->observations =
        malloc_or_die(sizeof(FitObservation) * (size_t)worker->capacity);
  }
  autoplay_args->move_observer = fit_observe_move;
  autoplay_args->move_observer_data = &shared;
  autoplay(autoplay_args, args->autoplay_results, error_stack);
  autoplay_args->move_observer = NULL;
  autoplay_args->move_observer_data = NULL;
  if (error_stack_is_empty(error_stack) &&
      thread_control_get_status(args->thread_control) !=
          THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    fit_finish(&shared, error_stack);
  }
  for (int worker_idx = 0; worker_idx < shared.number_of_workers;
       worker_idx++) {
    FitWorker *worker = &shared.workers[worker_idx];
    inference_ctx_destroy(worker->inference_ctx);
    inference_results_destroy(worker->inference_results);
    free(worker->observations);
  }
  free(shared.workers);
}
