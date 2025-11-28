#include "sim_results.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/game_defs.h"
#include "../def/sim_defs.h"
#include "../str/move_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bai_result.h"
#include "board.h"
#include "equity.h"
#include "game.h"
#include "letter_distribution.h"
#include "move.h"
#include "stats.h"
#include "win_pct.h"
#include "xoshiro.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct SimmedPlay {
  Move *move;
  Stat *score_stat[MAX_PLIES];
  Stat *bingo_stat[MAX_PLIES];
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  bool is_epigon;
  int play_id;
  XoshiroPRNG *prng;
  cpthread_mutex_t mutex;
};

struct SimResults {
  int num_plies;
  int num_simmed_plays;
  uint64_t iteration_count;
  cpthread_mutex_t simmed_plays_mutex;
  cpthread_mutex_t display_mutex;
  atomic_int node_count;
  SimmedPlay **simmed_plays;
  SimmedPlayDisplayInfo *simmed_play_display_infos;
  Rack rack;
  BAIResult *bai_result;
  bool valid_for_current_game_state;
};

SimmedPlay **simmed_plays_create(const MoveList *move_list,
                                 int num_simmed_plays, int num_plies,
                                 uint64_t seed) {
  SimmedPlay **simmed_plays =
      malloc_or_die((sizeof(SimmedPlay *)) * num_simmed_plays);

  for (int i = 0; i < num_simmed_plays; i++) {
    SimmedPlay *simmed_play = malloc_or_die(sizeof(SimmedPlay));
    simmed_play->move = move_create();
    move_copy(simmed_play->move, move_list_get_move(move_list, i));

    simmed_play->equity_stat = stat_create(true);
    simmed_play->leftover_stat = stat_create(true);
    simmed_play->win_pct_stat = stat_create(true);
    memset(simmed_play->bingo_stat, 0, sizeof(Stat *) * MAX_PLIES);
    memset(simmed_play->score_stat, 0, sizeof(Stat *) * MAX_PLIES);
    for (int j = 0; j < num_plies; j++) {
      simmed_play->score_stat[j] = stat_create(true);
      simmed_play->bingo_stat[j] = stat_create(true);
    }
    simmed_play->is_epigon = false;
    simmed_play->play_id = i;
    simmed_play->prng = prng_create(seed);
    cpthread_mutex_init(&simmed_play->mutex);
    simmed_plays[i] = simmed_play;
  }
  return simmed_plays;
}

void simmed_plays_destroy(SimmedPlay **simmed_plays, int num_simmed_plays,
                          int num_plies) {
  if (!simmed_plays) {
    return;
  }
  for (int i = 0; i < num_simmed_plays; i++) {
    for (int j = 0; j < num_plies; j++) {
      stat_destroy(simmed_plays[i]->bingo_stat[j]);
      stat_destroy(simmed_plays[i]->score_stat[j]);
    }
    stat_destroy(simmed_plays[i]->equity_stat);
    stat_destroy(simmed_plays[i]->leftover_stat);
    stat_destroy(simmed_plays[i]->win_pct_stat);
    move_destroy(simmed_plays[i]->move);
    prng_destroy(simmed_plays[i]->prng);
    free(simmed_plays[i]);
  }
  free(simmed_plays);
}

void sim_results_destroy_internal(SimResults *sim_results) {
  if (!sim_results) {
    return;
  }
  simmed_plays_destroy(sim_results->simmed_plays, sim_results->num_simmed_plays,
                       sim_results->num_plies);
  sim_results->simmed_plays = NULL;
  free(sim_results->simmed_play_display_infos);
  sim_results->simmed_play_display_infos = NULL;
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
  cpthread_mutex_lock(&sim_results->simmed_plays_mutex);
}

void sim_results_unlock_simmed_plays(SimResults *sim_results) {
  cpthread_mutex_unlock(&sim_results->simmed_plays_mutex);
}

void sim_results_reset(const MoveList *move_list, SimResults *sim_results,
                       int num_plies, uint64_t seed) {
  cpthread_mutex_lock(&sim_results->display_mutex);
  sim_results_destroy_internal(sim_results);

  const int num_simmed_plays = move_list_get_count(move_list);

  sim_results->simmed_plays =
      simmed_plays_create(move_list, num_simmed_plays, num_plies, seed);

  sim_results->simmed_play_display_infos =
      malloc_or_die(sizeof(SimmedPlayDisplayInfo) * num_simmed_plays);

  sim_results->num_simmed_plays = num_simmed_plays;
  sim_results->num_plies = num_plies;
  sim_results->iteration_count = 0;
  atomic_init(&sim_results->node_count, 0);
  sim_results->valid_for_current_game_state = false;
  cpthread_mutex_unlock(&sim_results->display_mutex);
}

SimResults *sim_results_create(void) {
  SimResults *sim_results = malloc_or_die(sizeof(SimResults));
  sim_results->num_simmed_plays = 0;
  sim_results->num_plies = 0;
  sim_results->iteration_count = 0;
  atomic_init(&sim_results->node_count, 0);
  cpthread_mutex_init(&sim_results->simmed_plays_mutex);
  cpthread_mutex_init(&sim_results->display_mutex);
  sim_results->simmed_plays = NULL;
  sim_results->simmed_play_display_infos = NULL;
  sim_results->bai_result = bai_result_create();
  sim_results->valid_for_current_game_state = false;
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
  cpthread_mutex_lock(&simmed_play->mutex);
  simmed_play->is_epigon = true;
  cpthread_mutex_unlock(&simmed_play->mutex);
}

// Returns the current seed and updates the seed using prng_next
uint64_t simmed_play_get_seed(SimmedPlay *simmed_play) {
  uint64_t seed;
  cpthread_mutex_lock(&simmed_play->mutex);
  seed = prng_next(simmed_play->prng);
  cpthread_mutex_unlock(&simmed_play->mutex);
  return seed;
}

int sim_results_get_number_of_plays(const SimResults *sim_results) {
  return sim_results->num_simmed_plays;
}

int sim_results_get_num_plies(const SimResults *sim_results) {
  return sim_results->num_plies;
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

const Rack *sim_results_get_rack(const SimResults *sim_results) {
  return &(sim_results->rack);
}

void sim_results_set_rack(SimResults *sim_results, const Rack *rack) {
  sim_results->rack = *rack;
}

BAIResult *sim_results_get_bai_result(const SimResults *sim_results) {
  return sim_results->bai_result;
}

void sim_results_increment_node_count(SimResults *sim_results) {
  atomic_fetch_add(&sim_results->node_count, 1);
}

void simmed_play_add_score_stat(SimmedPlay *simmed_play, Equity score,
                                bool is_bingo, int ply) {
  cpthread_mutex_lock(&simmed_play->mutex);
  stat_push(simmed_play->score_stat[ply], equity_to_double(score), 1);
  stat_push(simmed_play->bingo_stat[ply], (double)is_bingo, 1);
  cpthread_mutex_unlock(&simmed_play->mutex);
}

void simmed_play_add_equity_stat(SimmedPlay *simmed_play, Equity initial_spread,
                                 Equity spread, Equity leftover) {
  cpthread_mutex_lock(&simmed_play->mutex);
  stat_push(simmed_play->equity_stat,
            equity_to_double(spread - initial_spread + leftover), 1);
  stat_push(simmed_play->leftover_stat, equity_to_double(leftover), 1);
  cpthread_mutex_unlock(&simmed_play->mutex);
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
  cpthread_mutex_lock(&simmed_play->mutex);
  stat_push(simmed_play->win_pct_stat, wpct, 1);
  cpthread_mutex_unlock(&simmed_play->mutex);
  return wpct;
}

// NOT THREAD SAFE: Assumes no sim is in progress, the sim display info is
// updated and sorted, and n is in bounds.
void sim_results_get_nth_best_move(const SimResults *sim_results, int n,
                                   Move *move) {
  *move = sim_results->simmed_play_display_infos[n].move;
}

void sim_results_set_valid_for_current_game_state(SimResults *sim_results,
                                                  bool valid) {
  sim_results->valid_for_current_game_state = valid;
}

bool sim_results_get_valid_for_current_game_state(SimResults *sim_results) {
  return sim_results->valid_for_current_game_state;
}

void sim_results_write_to_display_info(SimResults *sim_results,
                                       const int simmed_play_index) {
  SimmedPlay *simmed_play =
      sim_results_get_simmed_play(sim_results, simmed_play_index);
  SimmedPlayDisplayInfo *simmed_play_display_info =
      &sim_results->simmed_play_display_infos[simmed_play_index];
  const int num_plies = sim_results_get_num_plies(sim_results);
  cpthread_mutex_lock(&simmed_play->mutex);
  move_copy(&simmed_play_display_info->move, simmed_play->move);
  for (int i = 0; i < num_plies; i++) {
    const Stat *bingo_stat = simmed_play_get_bingo_stat(simmed_play, i);
    const Stat *score_stat = simmed_play_get_score_stat(simmed_play, i);
    simmed_play_display_info->bingo_means[i] = stat_get_mean(bingo_stat);
    simmed_play_display_info->bingo_stdevs[i] = stat_get_stdev(bingo_stat);
    simmed_play_display_info->score_means[i] = stat_get_mean(score_stat);
    simmed_play_display_info->score_stdevs[i] = stat_get_stdev(score_stat);
  }
  simmed_play_display_info->equity_mean =
      stat_get_mean(simmed_play->equity_stat);
  simmed_play_display_info->equity_stdev =
      stat_get_stdev(simmed_play->equity_stat);
  simmed_play_display_info->win_pct_mean =
      stat_get_mean(simmed_play->win_pct_stat);
  simmed_play_display_info->win_pct_stdev =
      stat_get_stdev(simmed_play->win_pct_stat);
  simmed_play_display_info->is_epigon = simmed_play->is_epigon;
  simmed_play_display_info->niters =
      stat_get_num_samples(simmed_play->equity_stat);
  cpthread_mutex_unlock(&simmed_play->mutex);
}

int compare_simmed_play_display_infos(const void *a, const void *b) {
  const SimmedPlayDisplayInfo *play_a = (const SimmedPlayDisplayInfo *)a;
  const SimmedPlayDisplayInfo *play_b = (const SimmedPlayDisplayInfo *)b;

  if (play_a->is_epigon && !play_b->is_epigon) {
    return 1;
  }
  if (play_b->is_epigon && !play_a->is_epigon) {
    return -1;
  }

  // Compare the mean values of win_pct_stat
  if (play_a->win_pct_mean > play_b->win_pct_mean) {
    return -1;
  }
  if (play_a->win_pct_mean < play_b->win_pct_mean) {
    return 1;
  }
  // If win_pct_stat->mean values are equal, compare equity_stat->mean
  if (play_a->equity_mean > play_b->equity_mean) {
    return -1;
  }
  if (play_a->equity_mean < play_b->equity_mean) {
    return 1;
  }
  return 0;
}

// Returns true if the sim results are ready and the display infos have been
// updated and locked
// Returns false otherwise
bool sim_results_lock_and_sort_display_infos(SimResults *sim_results) {
  cpthread_mutex_lock(&sim_results->display_mutex);
  if (!sim_results->simmed_play_display_infos) {
    cpthread_mutex_unlock(&sim_results->display_mutex);
    return false;
  }

  int number_of_simmed_plays = sim_results_get_number_of_plays(sim_results);
  for (int i = 0; i < number_of_simmed_plays; i++) {
    sim_results_write_to_display_info(sim_results, i);
  }

  qsort(sim_results->simmed_play_display_infos, number_of_simmed_plays,
        sizeof(SimmedPlayDisplayInfo), compare_simmed_play_display_infos);
  return true;
}

void sim_results_unlock_display_infos(SimResults *sim_results) {
  cpthread_mutex_unlock(&sim_results->display_mutex);
}

SimmedPlayDisplayInfo *
sim_results_get_display_info(const SimResults *sim_results, int index) {
  return &sim_results->simmed_play_display_infos[index];
}