#include "pass_inference.h"

#include "../def/bit_rack_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/rack_value_table.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "census.h"
#include "census_play_scores.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  PASS_INFERENCE_INITIAL_CAPACITY = 4096,
  // The racks a thread claims at a time: each takes under a microsecond.
  PASS_INFERENCE_CLAIM_SIZE = 1024,
};

// One rack the passer could hold. A rack is kept as its rank in the census
// lattice, which orders full racks by their letter counts.
typedef struct PassInferenceRack {
  uint32_t lattice_index;
  Equity value;
  Equity best_equity;
  double mass;
} PassInferenceRack;

struct PassInference {
  CensusLattice *lattice;
  int num_letters;
  // The tiles the observer has not seen, and for each letter the number of
  // ways to draw each count of it from them.
  uint8_t pool[CENSUS_MAX_LETTERS];
  double ways[CENSUS_MAX_LETTERS][RACK_SIZE + 1];
  PassInferenceRack *racks;
  uint32_t number_of_racks;
  uint32_t capacity;
  double total_prior;
  double total_mass;
};

static void pass_inference_add_rack(PassInference *pass_inference,
                                    uint32_t lattice_index) {
  if (pass_inference->number_of_racks == pass_inference->capacity) {
    pass_inference->capacity = pass_inference->capacity == 0
                                   ? PASS_INFERENCE_INITIAL_CAPACITY
                                   : pass_inference->capacity * 2;
    pass_inference->racks =
        realloc_or_die(pass_inference->racks,
                       sizeof(PassInferenceRack) * pass_inference->capacity);
  }
  pass_inference->racks[pass_inference->number_of_racks].lattice_index =
      lattice_index;
  pass_inference->number_of_racks++;
}

// Adds every rack that takes `remaining` more tiles from the letters ml and
// after, in the order of their counts of ml, then of the next letter, and so
// on: the order of the lattice's ranks. pool_after[ml] is the number of pool
// tiles of those letters, and rank_within the sum of the rank terms
// (census_lattice_rank_term) of the letters before ml.
static void pass_inference_add_racks(PassInference *pass_inference,
                                     const int *pool_after, int ml,
                                     int remaining, uint32_t rank_within) {
  if (remaining == 0) {
    pass_inference_add_rack(pass_inference, census_lattice_get_full_rack_start(
                                                pass_inference->lattice) +
                                                rank_within);
    return;
  }
  if (ml == pass_inference->num_letters || pool_after[ml] < remaining) {
    return;
  }
  const int pool_count = pass_inference->pool[ml];
  const int max_count = pool_count < remaining ? pool_count : remaining;
  for (int count = 0; count <= max_count; count++) {
    // The last letter takes whatever is left and adds no term.
    const uint32_t rank_term =
        ml == pass_inference->num_letters - 1
            ? 0
            : census_lattice_rank_term(pass_inference->lattice,
                                       (MachineLetter)ml, remaining, count);
    pass_inference_add_racks(pass_inference, pool_after, ml + 1,
                             remaining - count, rank_within + rank_term);
  }
}

// One thread's share of the work: the play score units and then the racks it
// claims.
typedef struct PassInferenceWorker {
  PassInference *pass_inference;
  const Game *game;
  const RackValueTable *rack_value_table;
  const Equity *leave_values;
  Equity *play_scores;
  double temperature;
  double bonus;
  int number_of_workers;
  _Atomic int *next_unit;
  _Atomic uint32_t *next_rack;
} PassInferenceWorker;

static void *pass_inference_build_worker(void *uncasted_worker) {
  const PassInferenceWorker *worker =
      (const PassInferenceWorker *)uncasted_worker;
  census_play_scores_build_claimed(
      worker->game, worker->pass_inference->lattice,
      worker->pass_inference->pool, worker->play_scores,
      worker->number_of_workers > 1 ? worker->pass_inference->num_letters : 1,
      worker->next_unit, CENSUS_ALL_LANES);
  return NULL;
}

// The static equity of the rack's best play or exchange. The play scores
// start at 0, an exchange of the tiles, so every split of the rack into
// tiles used and tiles kept is worth its play score plus the leave value of
// the tiles kept.
static Equity pass_inference_get_rack_best_equity(
    const PassInferenceWorker *worker, const MachineLetter *letters,
    const uint8_t *counts, int number_of_letters) {
  CensusRackSplitter splitter;
  census_rack_splitter_init(&splitter, worker->pass_inference->lattice, letters,
                            counts, number_of_letters);
  uint32_t played_indexes[CENSUS_MAX_RACK_SPLITS];
  uint32_t kept_indexes[CENSUS_MAX_RACK_SPLITS];
  uint8_t tiles_played[CENSUS_MAX_RACK_SPLITS];
  const int number_of_splits = census_rack_splitter_next(
      &splitter, CENSUS_MAX_RACK_SPLITS, played_indexes, kept_indexes,
      tiles_played, NULL);
  Equity best_equity = EQUITY_MIN_VALUE;
  for (int split_idx = 0; split_idx < number_of_splits; split_idx++) {
    const Equity equity = worker->play_scores[played_indexes[split_idx]] +
                          worker->leave_values[kept_indexes[split_idx]];
    best_equity = equity > best_equity ? equity : best_equity;
  }
  return best_equity;
}

// Values one rack: the table's value, the best equity and the mass, which
// stays zero for a rack the table does not value.
static void pass_inference_value_rack(const PassInferenceWorker *worker,
                                      PassInferenceRack *rack) {
  const PassInference *pass_inference = worker->pass_inference;
  uint8_t tally[CENSUS_MAX_LETTERS];
  census_lattice_unrank(pass_inference->lattice, rack->lattice_index, tally);
  MachineLetter letters[RACK_SIZE];
  uint8_t counts[RACK_SIZE];
  int number_of_letters = 0;
  BitRack bit_rack = bit_rack_create_empty();
  double prior = 1.0;
  for (int ml = 0; ml < pass_inference->num_letters; ml++) {
    if (tally[ml] == 0) {
      continue;
    }
    letters[number_of_letters] = (MachineLetter)ml;
    counts[number_of_letters] = tally[ml];
    number_of_letters++;
    for (int copy_idx = 0; copy_idx < tally[ml]; copy_idx++) {
      bit_rack_add_letter(&bit_rack, (MachineLetter)ml);
    }
    prior *= pass_inference->ways[ml][tally[ml]];
  }
  rack->mass = 0.0;
  rack->best_equity = 0;
  if (!rack_value_table_lookup(worker->rack_value_table, &bit_rack,
                               &rack->value)) {
    rack->value = EQUITY_UNDEFINED_VALUE;
    return;
  }
  rack->best_equity = pass_inference_get_rack_best_equity(
      worker, letters, counts, number_of_letters);
  const double pass_probability =
      1.0 / (1.0 + exp((equity_to_double(rack->best_equity) -
                        equity_to_double(rack->value) - worker->bonus) /
                       worker->temperature));
  rack->mass = prior * pass_probability;
}

static void *pass_inference_rack_worker(void *uncasted_worker) {
  const PassInferenceWorker *worker =
      (const PassInferenceWorker *)uncasted_worker;
  PassInference *pass_inference = worker->pass_inference;
  const uint32_t number_of_racks = pass_inference->number_of_racks;
  while (true) {
    const uint32_t claim_start = atomic_fetch_add_explicit(
        worker->next_rack, PASS_INFERENCE_CLAIM_SIZE, memory_order_relaxed);
    if (claim_start >= number_of_racks) {
      return NULL;
    }
    const uint32_t claim_end =
        number_of_racks - claim_start < PASS_INFERENCE_CLAIM_SIZE
            ? number_of_racks
            : claim_start + PASS_INFERENCE_CLAIM_SIZE;
    for (uint32_t rack_idx = claim_start; rack_idx < claim_end; rack_idx++) {
      pass_inference_value_rack(worker, &pass_inference->racks[rack_idx]);
    }
  }
}

// Pushes the error and returns true if the pass cannot be inferred from.
static bool pass_inference_has_invalid_args(const PassInferenceArgs *args,
                                            ErrorStack *error_stack) {
  const Game *game = args->game;
  const LetterDistribution *ld = game_get_ld(game);
  const int num_letters = ld_get_size(ld);
  const RackValueTable *rack_value_table = args->rack_value_table;
  const char *unsupported = NULL;
  if (!(args->temperature > 0.0)) {
    unsupported = "the passer's temperature is not above zero";
  } else if (game_get_variant(game) != GAME_VARIANT_CLASSIC) {
    unsupported = "the game is not a classic game";
  } else if (!game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG)) {
    unsupported = "the players have different lexica";
  } else if (num_letters > BIT_RACK_MAX_ALPHABET_SIZE) {
    unsupported = "the letter distribution has too many letters";
  } else if (board_get_tiles_played(game_get_board(game)) == 0) {
    unsupported = "the board is empty";
  } else if (game_get_real_bag_size(game) < (RACK_SIZE)) {
    unsupported = "the bag is too small for the passer to have exchanged";
  }
  if (unsupported != NULL) {
    error_stack_push(
        error_stack, ERROR_STATUS_PASS_INFERENCE_UNSUPPORTED_GAME,
        get_formatted_string("cannot infer a rack from this pass: %s",
                             unsupported));
    return true;
  }
  bool same_distribution = rack_value_table->num_letters == num_letters;
  for (int ml = 0; same_distribution && ml < num_letters; ml++) {
    same_distribution = rack_value_table->letter_counts[ml] ==
                        ld_get_dist(ld, (MachineLetter)ml);
  }
  const int passer_index = game_get_player_on_turn_index(game);
  if (!same_distribution ||
      rack_value_table_get_klv_hash(rack_value_table) !=
          klv_get_hash(player_get_klv(game_get_player(game, passer_index)))) {
    error_stack_push(
        error_stack, ERROR_STATUS_PASS_INFERENCE_TABLE_MISMATCH,
        string_duplicate("cannot infer a rack from this pass: the rack value "
                         "table was made for another letter distribution or "
                         "other leaves than the passer's"));
    return true;
  }
  return false;
}

PassInference *pass_inference_create(const PassInferenceArgs *args,
                                     ErrorStack *error_stack) {
  if (pass_inference_has_invalid_args(args, error_stack)) {
    return NULL;
  }
  const Game *game = args->game;
  const int num_letters = ld_get_size(game_get_ld(game));
  CensusLattice *lattice =
      census_lattice_create(num_letters, RACK_SIZE, error_stack);
  if (lattice == NULL) {
    return NULL;
  }
  PassInference *pass_inference = calloc_or_die(1, sizeof(PassInference));
  pass_inference->lattice = lattice;
  pass_inference->num_letters = num_letters;

  // The passer could hold any tiles of the bag and of the rack the game has
  // them holding.
  const int passer_index = game_get_player_on_turn_index(game);
  const Player *passer = game_get_player(game, passer_index);
  const Rack *passer_rack = player_get_rack(passer);
  const Bag *bag = game_get_bag(game);
  int pool_after[CENSUS_MAX_LETTERS + 1];
  pool_after[num_letters] = 0;
  for (int ml = num_letters - 1; ml >= 0; ml--) {
    const int pool_count = bag_get_letter(bag, (MachineLetter)ml) +
                           rack_get_letter(passer_rack, (MachineLetter)ml);
    pass_inference->pool[ml] = (uint8_t)pool_count;
    pool_after[ml] = pool_after[ml + 1] + pool_count;
    double ways = 1.0;
    for (int count = 0; count <= (RACK_SIZE); count++) {
      pass_inference->ways[ml][count] = ways;
      ways = ways * (double)(pool_count - count) / (double)(count + 1);
    }
  }
  pass_inference_add_racks(pass_inference, pool_after, 0, RACK_SIZE, 0);

  const uint32_t lattice_size = census_lattice_get_size(lattice);
  Equity *leave_values = malloc_or_die(sizeof(Equity) * lattice_size);
  census_leaves_from_klv(lattice, player_get_klv(passer), leave_values);
  // Every play score starts at 0, the score of exchanging the tiles: the bag
  // lets the passer exchange any of them.
  Equity *play_scores = calloc_or_die(lattice_size, sizeof(Equity));

  const int number_of_workers = args->num_threads < 1 ? 1 : args->num_threads;
  _Atomic int next_unit;
  atomic_init(&next_unit, 0);
  _Atomic uint32_t next_rack;
  atomic_init(&next_rack, 0);
  PassInferenceWorker *workers =
      malloc_or_die(sizeof(PassInferenceWorker) * (size_t)number_of_workers);
  for (int worker_idx = 0; worker_idx < number_of_workers; worker_idx++) {
    workers[worker_idx] = (PassInferenceWorker){
        .pass_inference = pass_inference,
        .game = game,
        .rack_value_table = args->rack_value_table,
        .leave_values = leave_values,
        .play_scores = play_scores,
        .temperature = args->temperature,
        .bonus = args->bonus,
        .number_of_workers = number_of_workers,
        .next_unit = &next_unit,
        .next_rack = &next_rack,
    };
  }
  census_run_phase(workers, sizeof(PassInferenceWorker), number_of_workers,
                   pass_inference_build_worker);
  census_run_phase(workers, sizeof(PassInferenceWorker), number_of_workers,
                   pass_inference_rack_worker);
  free(workers);
  free(play_scores);
  free(leave_values);

  // Drop the racks the table does not value, and sum the rest in their
  // order, so that the totals do not depend on the threads.
  uint32_t number_of_valued_racks = 0;
  for (uint32_t rack_idx = 0; rack_idx < pass_inference->number_of_racks;
       rack_idx++) {
    const PassInferenceRack *rack = &pass_inference->racks[rack_idx];
    if (rack->value == EQUITY_UNDEFINED_VALUE) {
      continue;
    }
    pass_inference->racks[number_of_valued_racks] = *rack;
    pass_inference->total_prior +=
        pass_inference_get_prior(pass_inference, number_of_valued_racks);
    pass_inference->total_mass += rack->mass;
    number_of_valued_racks++;
  }
  pass_inference->number_of_racks = number_of_valued_racks;
  return pass_inference;
}

void pass_inference_destroy(PassInference *pass_inference) {
  if (!pass_inference) {
    return;
  }
  census_lattice_destroy(pass_inference->lattice);
  free(pass_inference->racks);
  free(pass_inference);
}

uint32_t
pass_inference_get_number_of_racks(const PassInference *pass_inference) {
  return pass_inference->number_of_racks;
}

void pass_inference_get_rack(const PassInference *pass_inference,
                             uint32_t rack_idx, Rack *rack) {
  uint8_t tally[CENSUS_MAX_LETTERS];
  census_lattice_unrank(pass_inference->lattice,
                        pass_inference->racks[rack_idx].lattice_index, tally);
  rack_set_dist_size_and_reset(rack, pass_inference->num_letters);
  for (int ml = 0; ml < pass_inference->num_letters; ml++) {
    if (tally[ml] > 0) {
      rack_add_letters(rack, (MachineLetter)ml, tally[ml]);
    }
  }
}

uint32_t pass_inference_find_rack(const PassInference *pass_inference,
                                  const Rack *rack) {
  if (rack_get_dist_size(rack) != pass_inference->num_letters ||
      rack_get_total_letters(rack) != (RACK_SIZE)) {
    return UINT32_MAX;
  }
  uint8_t tally[CENSUS_MAX_LETTERS] = {0};
  for (int ml = 0; ml < pass_inference->num_letters; ml++) {
    const int count = rack_get_letter(rack, (MachineLetter)ml);
    if (count > pass_inference->pool[ml]) {
      return UINT32_MAX;
    }
    tally[ml] = (uint8_t)count;
  }
  const uint32_t lattice_index =
      census_lattice_rank(pass_inference->lattice, tally);
  // The racks are in the order of their lattice indexes.
  uint32_t low = 0;
  uint32_t high = pass_inference->number_of_racks;
  while (low < high) {
    const uint32_t middle = low + ((high - low) / 2);
    if (pass_inference->racks[middle].lattice_index < lattice_index) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low < pass_inference->number_of_racks &&
      pass_inference->racks[low].lattice_index == lattice_index) {
    return low;
  }
  return UINT32_MAX;
}

Equity pass_inference_get_value(const PassInference *pass_inference,
                                uint32_t rack_idx) {
  return pass_inference->racks[rack_idx].value;
}

Equity pass_inference_get_best_equity(const PassInference *pass_inference,
                                      uint32_t rack_idx) {
  return pass_inference->racks[rack_idx].best_equity;
}

double pass_inference_get_prior(const PassInference *pass_inference,
                                uint32_t rack_idx) {
  uint8_t tally[CENSUS_MAX_LETTERS];
  census_lattice_unrank(pass_inference->lattice,
                        pass_inference->racks[rack_idx].lattice_index, tally);
  double prior = 1.0;
  for (int ml = 0; ml < pass_inference->num_letters; ml++) {
    prior *= pass_inference->ways[ml][tally[ml]];
  }
  return prior;
}

double pass_inference_get_mass(const PassInference *pass_inference,
                               uint32_t rack_idx) {
  return pass_inference->racks[rack_idx].mass;
}

double pass_inference_get_total_prior(const PassInference *pass_inference) {
  return pass_inference->total_prior;
}

double pass_inference_get_total_mass(const PassInference *pass_inference) {
  return pass_inference->total_mass;
}

double pass_inference_get_probability(const PassInference *pass_inference,
                                      uint32_t rack_idx) {
  return pass_inference->racks[rack_idx].mass / pass_inference->total_mass;
}
