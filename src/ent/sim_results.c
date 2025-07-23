#include "sim_results.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/game_defs.h"

#include "bai_result.h"
#include "equity.h"
#include "move.h"
#include "stats.h"
#include "win_pct.h"
#include "xoshiro.h"

#include "../util/io_util.h"

struct SimmedPlay {
  Move *move;
  Stat **score_stat;
  Stat **bingo_stat;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  bool is_epigon;
  int play_id;
  XoshiroPRNG *prng;
  pthread_mutex_t mutex;
};

struct SimResults {
  int max_plies;
  int num_simmed_plays;
  uint64_t iteration_count;
  bool simmed_plays_initialized;
  pthread_mutex_t simmed_plays_mutex;
  atomic_int node_count;
  SimmedPlay **simmed_plays;
  SimmedPlay **sorted_simmed_plays;
  BAIResult *bai_result;
};

SimmedPlay **simmed_plays_create(const MoveList *move_list,
                                 uint32_t num_simmed_plays, int max_plies,
                                 uint64_t seed) {
  SimmedPlay **simmed_plays =
      malloc_or_die((sizeof(SimmedPlay)) * num_simmed_plays);

  for (uint32_t i = 0; i < num_simmed_plays; i++) {
    SimmedPlay *simmed_play = malloc_or_die(sizeof(SimmedPlay));
    simmed_play->move = move_create();
    move_copy(simmed_play->move, move_list_get_move(move_list, i));

    simmed_play->score_stat = malloc_or_die(sizeof(Stat *) * max_plies);
    simmed_play->bingo_stat = malloc_or_die(sizeof(Stat *) * max_plies);
    simmed_play->equity_stat = stat_create(true);
    simmed_play->leftover_stat = stat_create(true);
    simmed_play->win_pct_stat = stat_create(true);
    for (int j = 0; j < max_plies; j++) {
      simmed_play->score_stat[j] = stat_create(true);
      simmed_play->bingo_stat[j] = stat_create(true);
    }
    simmed_play->is_epigon = false;
    simmed_play->play_id = i;
    simmed_play->prng = prng_create(seed);
    pthread_mutex_init(&simmed_play->mutex, NULL);
    simmed_plays[i] = simmed_play;
  }
  return simmed_plays;
}

void simmed_plays_destroy(SimmedPlay **simmed_plays, int num_simmed_plays,
                          int max_plies) {
  if (!simmed_plays) {
    return;
  }
  for (int i = 0; i < num_simmed_plays; i++) {
    for (int j = 0; j < max_plies; j++) {
      stat_destroy(simmed_plays[i]->bingo_stat[j]);
      stat_destroy(simmed_plays[i]->score_stat[j]);
    }
    free(simmed_plays[i]->bingo_stat);
    free(simmed_plays[i]->score_stat);
    stat_destroy(simmed_plays[i]->equity_stat);
    stat_destroy(simmed_plays[i]->leftover_stat);
    stat_destroy(simmed_plays[i]->win_pct_stat);
    move_destroy(simmed_plays[i]->move);
    prng_destroy(simmed_plays[i]->prng);
    pthread_mutex_destroy(&simmed_plays[i]->mutex);
    free(simmed_plays[i]);
  }
  free(simmed_plays);
}

void sim_results_destroy_internal(SimResults *sim_results) {
  if (!sim_results) {
    return;
  }
  simmed_plays_destroy(sim_results->simmed_plays, sim_results->num_simmed_plays,
                       sim_results->max_plies);
  sim_results->simmed_plays = NULL;
  free(sim_results->sorted_simmed_plays);
  sim_results->sorted_simmed_plays = NULL;
}

void sim_results_destroy(SimResults *sim_results) {
  if (!sim_results) {
    return;
  }
  sim_results_destroy_internal(sim_results);
  bai_result_destroy(sim_results->bai_result);
  free(sim_results);
}

void sim_results_lock_simmed_plays(SimResults *sim_results) {
  pthread_mutex_lock(&sim_results->simmed_plays_mutex);
}

void sim_results_unlock_simmed_plays(SimResults *sim_results) {
  pthread_mutex_unlock(&sim_results->simmed_plays_mutex);
}

bool sim_results_get_simmed_plays_initialized(SimResults *sim_results) {
  bool value;
  sim_results_lock_simmed_plays(sim_results);
  value = sim_results->simmed_plays_initialized;
  sim_results_unlock_simmed_plays(sim_results);
  return value;
}

void sim_results_set_simmed_plays_initialized(SimResults *sim_results,
                                              bool value) {
  sim_results_lock_simmed_plays(sim_results);
  sim_results->simmed_plays_initialized = value;
  sim_results_unlock_simmed_plays(sim_results);
}

void sim_results_reset(const MoveList *move_list, SimResults *sim_results,
                       int max_plies, uint64_t seed) {
  sim_results_destroy_internal(sim_results);

  const uint32_t num_simmed_plays = move_list_get_count(move_list);

  sim_results->simmed_plays =
      simmed_plays_create(move_list, num_simmed_plays, max_plies, seed);

  // Copy simmed_plays to sorted_simmed_plays
  sim_results->sorted_simmed_plays =
      malloc_or_die(sizeof(SimmedPlay *) * num_simmed_plays);
  for (uint32_t i = 0; i < num_simmed_plays; i++) {
    sim_results->sorted_simmed_plays[i] = sim_results->simmed_plays[i];
  }

  sim_results->num_simmed_plays = num_simmed_plays;
  sim_results->max_plies = max_plies;
  sim_results->iteration_count = 0;
  atomic_init(&sim_results->node_count, 0);
  sim_results_set_simmed_plays_initialized(sim_results, true);
}

SimResults *sim_results_create(void) {
  SimResults *sim_results = malloc_or_die(sizeof(SimResults));
  sim_results->num_simmed_plays = 0;
  sim_results->max_plies = 0;
  sim_results->iteration_count = 0;
  atomic_init(&sim_results->node_count, 0);
  sim_results->simmed_plays_initialized = false;
  pthread_mutex_init(&sim_results->simmed_plays_mutex, NULL);
  sim_results->simmed_plays = NULL;
  sim_results->sorted_simmed_plays = NULL;
  sim_results->bai_result = bai_result_create();
  return sim_results;
}

Move *simmed_play_get_move(const SimmedPlay *simmed_play) {
  return simmed_play->move;
}

Stat *simmed_play_get_score_stat(const SimmedPlay *simmed_play,
                                 int stat_index) {
  return simmed_play->score_stat[stat_index];
}

Stat *simmed_play_get_bingo_stat(const SimmedPlay *simmed_play,
                                 int stat_index) {
  return simmed_play->bingo_stat[stat_index];
}

Stat *simmed_play_get_equity_stat(const SimmedPlay *simmed_play) {
  return simmed_play->equity_stat;
}

Stat *simmed_play_get_win_pct_stat(const SimmedPlay *simmed_play) {
  return simmed_play->win_pct_stat;
}

bool simmed_play_get_is_epigon(const SimmedPlay *simmed_play) {
  return simmed_play->is_epigon;
}

int simmed_play_get_id(const SimmedPlay *simmed_play) {
  return simmed_play->play_id;
}

void simmed_play_set_is_epigon(SimmedPlay *simmed_play) {
  pthread_mutex_lock(&simmed_play->mutex);
  simmed_play->is_epigon = true;
  pthread_mutex_unlock(&simmed_play->mutex);
}

// Returns the current seed and updates the seed using prng_next
uint64_t simmed_play_get_seed(SimmedPlay *simmed_play) {
  uint64_t seed;
  pthread_mutex_lock(&simmed_play->mutex);
  seed = prng_next(simmed_play->prng);
  pthread_mutex_unlock(&simmed_play->mutex);
  return seed;
}

int sim_results_get_number_of_plays(const SimResults *sim_results) {
  return sim_results->num_simmed_plays;
}

int sim_results_get_max_plies(const SimResults *sim_results) {
  return sim_results->max_plies;
}

int sim_results_get_node_count(const SimResults *sim_results) {
  return atomic_load(&sim_results->node_count);
}

// Not thread safe. Caller is responsible for ensuring thread
// safety
uint64_t sim_results_get_iteration_count(const SimResults *sim_results) {
  return sim_results->iteration_count;
}

// Not thread safe. Caller is responsible for ensuring thread
// safety
void sim_results_set_iteration_count(SimResults *sim_results, uint64_t count) {
  sim_results->iteration_count = count;
}

SimmedPlay *sim_results_get_simmed_play(const SimResults *sim_results,
                                        const int index) {
  return sim_results->simmed_plays[index];
}

SimmedPlay *sim_results_get_sorted_simmed_play(SimResults *sim_results,
                                               int index) {
  return sim_results->sorted_simmed_plays[index];
}

BAIResult *sim_results_get_bai_result(const SimResults *sim_results) {
  return sim_results->bai_result;
}

void sim_results_increment_node_count(SimResults *sim_results) {
  atomic_fetch_add(&sim_results->node_count, 1);
}

void simmed_play_add_score_stat(SimmedPlay *simmed_play, Equity score,
                                bool is_bingo, int ply) {
  pthread_mutex_lock(&simmed_play->mutex);
  stat_push(simmed_play->score_stat[ply], equity_to_double(score), 1);
  stat_push(simmed_play->bingo_stat[ply], (double)is_bingo, 1);
  pthread_mutex_unlock(&simmed_play->mutex);
}

void simmed_play_add_equity_stat(SimmedPlay *simmed_play, Equity initial_spread,
                                 Equity spread, Equity leftover) {
  pthread_mutex_lock(&simmed_play->mutex);
  stat_push(simmed_play->equity_stat,
            equity_to_double(spread - initial_spread + leftover), 1);
  stat_push(simmed_play->leftover_stat, equity_to_double(leftover), 1);
  pthread_mutex_unlock(&simmed_play->mutex);
}

int round_to_nearest_int(double a) {
  return (int)(a + 0.5 - (a < 0)); // truncated to 55
}

double simmed_play_add_win_pct_stat(const WinPct *wp, SimmedPlay *simmed_play,
                                    Equity spread, Equity leftover,
                                    game_end_reason_t game_end_reason,
                                    int game_unseen_tiles, bool plies_are_odd) {
  double wpct = 0.0;
  if (game_end_reason != GAME_END_REASON_NONE) {
    // the game ended; use the actual result.
    if (spread == 0) {
      wpct = 0.5;
    } else if (spread > 0) {
      wpct = 1.0;
    }
  } else {
    int spread_plus_leftover =
        round_to_nearest_int(equity_to_double(spread + leftover));
    // for an even-ply sim, it is our opponent's turn at the end of the sim.
    // the table is calculated from our perspective, so flip the spread.
    // i.e. if we are winning by 20 pts at the end of the sim, and our opponent
    // is on turn, we want to look up -20 as the spread, and then flip the win %
    // as well.
    if (!plies_are_odd) {
      spread_plus_leftover = -spread_plus_leftover;
    }
    wpct = (double)win_pct_get(wp, spread_plus_leftover, game_unseen_tiles);
    if (!plies_are_odd) {
      // see above comment regarding flipping win%
      wpct = 1.0 - wpct;
    }
  }
  pthread_mutex_lock(&simmed_play->mutex);
  stat_push(simmed_play->win_pct_stat, wpct, 1);
  pthread_mutex_unlock(&simmed_play->mutex);
  return wpct;
}

int compare_simmed_plays(const void *a, const void *b) {
  const SimmedPlay *play_a = *(const SimmedPlay **)a;
  const SimmedPlay *play_b = *(const SimmedPlay **)b;

  if (play_a->is_epigon && !play_b->is_epigon) {
    return 1;
  }
  if (play_b->is_epigon && !play_a->is_epigon) {
    return -1;
  }

  // Compare the mean values of win_pct_stat
  double mean_a = stat_get_mean(play_a->win_pct_stat);
  double mean_b = stat_get_mean(play_b->win_pct_stat);

  if (mean_a > mean_b) {
    return -1;
  }
  if (mean_a < mean_b) {
    return 1;
  }
  // If win_pct_stat->mean values are equal, compare equity_stat->mean
  double equity_mean_a = stat_get_mean(play_a->equity_stat);
  double equity_mean_b = stat_get_mean(play_b->equity_stat);

  if (equity_mean_a > equity_mean_b) {
    return -1;
  }
  if (equity_mean_a < equity_mean_b) {
    return 1;
  }
  return 0;
}

// Sorts the sorted_simmed_plays and leaves the simmed_plays unchanged.
// Returns true if the plays were successfully sorted.
// Returns false if the plays have not been created yet.
// Not thread safe. Must be called with a lock on the simmed plays mutex
// during multithreaded simming.
bool sim_results_sort_plays_by_win_rate(SimResults *sim_results) {
  if (!sim_results->simmed_plays || !sim_results->sorted_simmed_plays) {
    return false;
  }
  qsort(sim_results->sorted_simmed_plays, sim_results->num_simmed_plays,
        sizeof(SimmedPlay *), compare_simmed_plays);
  return true;
}