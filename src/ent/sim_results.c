#include "sim_results.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "bai_result.h"
#include "equity.h"
#include "heat_map.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
#include "win_pct.h"
#include "xoshiro.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct PlyInfo {
  Stat *score_stat;
  Stat *bingo_stat;
  HeatMap *heat_map;
  uint64_t ply_info_counts[NUM_PLY_INFO_COUNT_TYPES];
} PlyInfo;

struct SimmedPlay {
  Move move;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  uint64_t similarity_key;
  int play_index_by_sort_type;
  XoshiroPRNG *prng;
  PlyInfo *ply_infos;
  cpthread_mutex_t mutex;
};

struct SimResults {
  int num_plies;
  int num_alloc_plies;
  int num_simmed_plays;
  int num_alloc_simmed_plays;
  atomic_uint_least64_t iteration_count;
  atomic_uint_least64_t node_count;
  cpthread_mutex_t simmed_plays_mutex;
  cpthread_mutex_t display_mutex;
  SimmedPlay **simmed_plays;
  SimmedPlay **display_simmed_plays;
  Rack rack;
  Rack known_opp_rack;
  BAIResult *bai_result;
  bool valid_for_current_game_state;
};

void ply_info_init(PlyInfo *ply_info, bool use_heat_map) {
  ply_info->score_stat = stat_create(true);
  ply_info->bingo_stat = stat_create(true);
  if (use_heat_map) {
    ply_info->heat_map = heat_map_create();
  } else {
    ply_info->heat_map = NULL;
  }
  memset(ply_info->ply_info_counts, 0, sizeof(ply_info->ply_info_counts));
}

void ply_info_reset(PlyInfo *ply_info, bool use_heat_map) {
  stat_reset(ply_info->score_stat);
  stat_reset(ply_info->bingo_stat);
  if (use_heat_map) {
    if (!ply_info->heat_map) {
      ply_info->heat_map = heat_map_create();
    } else {
      heat_map_reset(ply_info->heat_map);
    }
  } else {
    heat_map_destroy(ply_info->heat_map);
    ply_info->heat_map = NULL;
  }
  memset(ply_info->ply_info_counts, 0, sizeof(ply_info->ply_info_counts));
}

SimmedPlay *simmed_play_create(const MoveList *move_list, int num_plies,
                               uint64_t seed, bool use_heat_map, const int i) {
  SimmedPlay *simmed_play = malloc_or_die(sizeof(SimmedPlay));
  move_copy(&simmed_play->move, move_list_get_move(move_list, i));
  simmed_play->equity_stat = stat_create(true);
  simmed_play->leftover_stat = stat_create(true);
  simmed_play->win_pct_stat = stat_create(true);
  simmed_play->ply_infos = malloc_or_die(sizeof(PlyInfo) * num_plies);
  for (int j = 0; j < num_plies; j++) {
    ply_info_init(&simmed_play->ply_infos[j], use_heat_map);
  }
  simmed_play->similarity_key = 0;
  simmed_play->play_index_by_sort_type = i;
  simmed_play->prng = prng_create(seed);
  cpthread_mutex_init(&simmed_play->mutex);
  return simmed_play;
}

SimmedPlay *simmed_play_reset(SimmedPlay *simmed_play,
                              const MoveList *move_list,
                              int old_num_alloc_plies, int new_num_alloc_plies,
                              int num_plies, uint64_t seed, bool use_heat_map,
                              const int i) {
  move_copy(&simmed_play->move, move_list_get_move(move_list, i));
  stat_reset(simmed_play->equity_stat);
  stat_reset(simmed_play->leftover_stat);
  stat_reset(simmed_play->win_pct_stat);
  if (new_num_alloc_plies > old_num_alloc_plies) {
    simmed_play->ply_infos = realloc_or_die(
        simmed_play->ply_infos, sizeof(PlyInfo) * new_num_alloc_plies);
    for (int j = old_num_alloc_plies; j < new_num_alloc_plies; j++) {
      ply_info_init(&simmed_play->ply_infos[j], use_heat_map);
    }
  }
  for (int j = 0; j < num_plies && j < old_num_alloc_plies; j++) {
    ply_info_reset(&simmed_play->ply_infos[j], use_heat_map);
  }
  simmed_play->similarity_key = 0;
  simmed_play->play_index_by_sort_type = i;
  prng_seed(simmed_play->prng, seed);
  return simmed_play;
}

SimmedPlay **create_simmed_plays_array(const MoveList *move_list,
                                       const int num_simmed_plays,
                                       const int num_plies, const uint64_t seed,
                                       const bool use_heat_map) {
  SimmedPlay **simmed_plays =
      malloc_or_die((sizeof(SimmedPlay *)) * num_simmed_plays);
  for (int i = 0; i < num_simmed_plays; i++) {
    simmed_plays[i] =
        simmed_play_create(move_list, num_plies, seed, use_heat_map, i);
  }
  return simmed_plays;
}

SimmedPlay **
realloc_simmed_plays_array(SimmedPlay **simmed_plays, const MoveList *move_list,
                           const int old_num_alloc_sps,
                           const int new_num_alloc_sps, const int num_plies,
                           const uint64_t seed, const bool use_heat_map) {
  simmed_plays =
      realloc_or_die(simmed_plays, (sizeof(SimmedPlay *)) * new_num_alloc_sps);
  for (int i = old_num_alloc_sps; i < new_num_alloc_sps; i++) {
    simmed_plays[i] =
        simmed_play_create(move_list, num_plies, seed, use_heat_map, i);
  }
  return simmed_plays;
}

void sim_results_create_simmed_plays(SimResults *sim_results,
                                     const MoveList *move_list, int num_plies,
                                     uint64_t seed, bool use_heat_map) {
  const int num_simmed_plays = move_list_get_count(move_list);
  sim_results->num_simmed_plays = num_simmed_plays;
  sim_results->num_alloc_simmed_plays = num_simmed_plays;
  sim_results->num_plies = num_plies;
  sim_results->num_alloc_plies = num_plies;
  // FIXME: ensure heatmaps are off for sim autoplay
  sim_results->simmed_plays = create_simmed_plays_array(
      move_list, num_simmed_plays, num_plies, seed, use_heat_map);
  // FIXME: don't create display simmed plays for sim autoplay
  sim_results->display_simmed_plays = create_simmed_plays_array(
      move_list, num_simmed_plays, num_plies, 0, false);
}

void sim_results_simmed_plays_reset(SimResults *sim_results,
                                    const MoveList *move_list, int num_plies,
                                    uint64_t seed, bool use_heat_map) {
  const int old_num_alloc_sps = sim_results->num_alloc_simmed_plays;
  const int new_num_sps = move_list_get_count(move_list);
  sim_results->num_simmed_plays = new_num_sps;
  if (new_num_sps > old_num_alloc_sps) {
    sim_results->simmed_plays = realloc_simmed_plays_array(
        sim_results->simmed_plays, move_list, old_num_alloc_sps, new_num_sps,
        num_plies, seed, use_heat_map);
    sim_results->display_simmed_plays = realloc_simmed_plays_array(
        sim_results->display_simmed_plays, move_list, old_num_alloc_sps,
        new_num_sps, num_plies, 0, false);
    sim_results->num_alloc_simmed_plays = new_num_sps;
  }
  sim_results->num_plies = num_plies;
  const int old_num_alloc_plies = sim_results->num_alloc_plies;
  int new_num_alloc_plies = sim_results->num_alloc_plies;
  if (num_plies > sim_results->num_alloc_plies) {
    sim_results->num_alloc_plies = num_plies;
    new_num_alloc_plies = num_plies;
  }
  // If num_plies grew, resize all previously allocated plays to ensure
  // their ply_infos arrays are reallocated. Otherwise, only reset active plays.
  int num_plays_to_reset =
      new_num_sps < old_num_alloc_sps ? new_num_sps : old_num_alloc_sps;
  if (num_plies > old_num_alloc_plies) {
    num_plays_to_reset = old_num_alloc_sps;
  }
  for (int i = 0; i < num_plays_to_reset; i++) {
    simmed_play_reset(sim_results->simmed_plays[i], move_list,
                      old_num_alloc_plies, new_num_alloc_plies, num_plies, seed,
                      use_heat_map, i);
    simmed_play_reset(sim_results->display_simmed_plays[i], move_list,
                      old_num_alloc_plies, new_num_alloc_plies, num_plies, 0,
                      false, i);
  }
}

// Does not copy the following fields:
// - PRNG
// - mutex
// - heat_map
void simmed_play_copy(SimmedPlay *dst, const SimmedPlay *src,
                      const int num_plies) {
  move_copy(&dst->move, &src->move);
  stat_copy(dst->equity_stat, src->equity_stat);
  stat_copy(dst->leftover_stat, src->leftover_stat);
  stat_copy(dst->win_pct_stat, src->win_pct_stat);
  dst->similarity_key = src->similarity_key;
  dst->play_index_by_sort_type = src->play_index_by_sort_type;
  for (int i = 0; i < num_plies; i++) {
    stat_copy(dst->ply_infos[i].score_stat, src->ply_infos[i].score_stat);
    stat_copy(dst->ply_infos[i].bingo_stat, src->ply_infos[i].bingo_stat);
    memcpy(dst->ply_infos[i].ply_info_counts, src->ply_infos[i].ply_info_counts,
           sizeof(dst->ply_infos[i].ply_info_counts));
  }
}

void simmed_plays_destroy(SimmedPlay **simmed_plays, int num_alloc_sps,
                          int num_alloc_plies) {
  if (!simmed_plays) {
    return;
  }
  for (int i = 0; i < num_alloc_sps; i++) {
    for (int j = 0; j < num_alloc_plies; j++) {
      stat_destroy(simmed_plays[i]->ply_infos[j].bingo_stat);
      stat_destroy(simmed_plays[i]->ply_infos[j].score_stat);
      heat_map_destroy(simmed_plays[i]->ply_infos[j].heat_map);
    }
    free(simmed_plays[i]->ply_infos);
    stat_destroy(simmed_plays[i]->equity_stat);
    stat_destroy(simmed_plays[i]->leftover_stat);
    stat_destroy(simmed_plays[i]->win_pct_stat);
    prng_destroy(simmed_plays[i]->prng);
    free(simmed_plays[i]);
  }
  free(simmed_plays);
}

void sim_results_destroy(SimResults *sim_results) {
  if (!sim_results) {
    return;
  }
  simmed_plays_destroy(sim_results->simmed_plays,
                       sim_results->num_alloc_simmed_plays,
                       sim_results->num_alloc_plies);
  simmed_plays_destroy(sim_results->display_simmed_plays,
                       sim_results->num_alloc_simmed_plays,
                       sim_results->num_alloc_plies);
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
                       int num_plies, uint64_t seed, bool use_heat_map) {
  cpthread_mutex_lock(&sim_results->display_mutex);
  if (!sim_results->simmed_plays) {
    sim_results_create_simmed_plays(sim_results, move_list, num_plies, seed,
                                    use_heat_map);
  } else {
    sim_results_simmed_plays_reset(sim_results, move_list, num_plies, seed,
                                   use_heat_map);
  }
  atomic_init(&sim_results->node_count, 0);
  atomic_init(&sim_results->iteration_count, 0);
  sim_results->valid_for_current_game_state = false;
  cpthread_mutex_unlock(&sim_results->display_mutex);
}

SimResults *sim_results_create(void) {
  SimResults *sim_results = malloc_or_die(sizeof(SimResults));
  sim_results->num_simmed_plays = 0;
  sim_results->num_alloc_simmed_plays = 0;
  sim_results->num_plies = 0;
  sim_results->num_alloc_plies = 0;
  atomic_init(&sim_results->node_count, 0);
  atomic_init(&sim_results->iteration_count, 0);
  cpthread_mutex_init(&sim_results->simmed_plays_mutex);
  cpthread_mutex_init(&sim_results->display_mutex);
  sim_results->simmed_plays = NULL;
  sim_results->display_simmed_plays = NULL;
  sim_results->bai_result = bai_result_create();
  sim_results->valid_for_current_game_state = false;
  rack_set_dist_size_and_reset(&sim_results->rack, 0);
  rack_set_dist_size_and_reset(&sim_results->known_opp_rack, 0);
  return sim_results;
}

const Move *simmed_play_get_move(const SimmedPlay *simmed_play) {
  return &simmed_play->move;
}

const Stat *simmed_play_get_score_stat(const SimmedPlay *simmed_play,
                                       int ply_index) {
  return simmed_play->ply_infos[ply_index].score_stat;
}

const Stat *simmed_play_get_bingo_stat(const SimmedPlay *simmed_play,
                                       int ply_index) {
  return simmed_play->ply_infos[ply_index].bingo_stat;
}

HeatMap *simmed_play_get_heat_map(SimmedPlay *simmed_play, int ply_index) {
  return simmed_play->ply_infos[ply_index].heat_map;
}

uint64_t simmed_play_get_ply_info_count(const SimmedPlay *simmed_play,
                                        int ply_index,
                                        ply_info_count_t count_type) {
  return simmed_play->ply_infos[ply_index].ply_info_counts[count_type];
}

const Stat *simmed_play_get_equity_stat(const SimmedPlay *simmed_play) {
  return simmed_play->equity_stat;
}

const Stat *simmed_play_get_win_pct_stat(const SimmedPlay *simmed_play) {
  return simmed_play->win_pct_stat;
}

int simmed_play_get_play_index_by_sort_type(const SimmedPlay *simmed_play) {
  return simmed_play->play_index_by_sort_type;
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

uint64_t sim_results_get_node_count(const SimResults *sim_results) {
  return atomic_load(&sim_results->node_count);
}

void sim_results_increment_node_count(SimResults *sim_results) {
  atomic_fetch_add(&sim_results->node_count, 1);
}

uint64_t sim_results_get_iteration_count(const SimResults *sim_results) {
  return atomic_load(&sim_results->iteration_count);
}

void sim_results_increment_iteration_count(SimResults *sim_results) {
  atomic_fetch_add(&sim_results->iteration_count, 1);
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

const Rack *sim_results_get_known_opp_rack(const SimResults *sim_results) {
  return &(sim_results->known_opp_rack);
}

void sim_results_set_known_opp_rack(SimResults *sim_results,
                                    const Rack *known_opp_rack) {
  if (!known_opp_rack) {
    rack_set_dist_size_and_reset(&sim_results->known_opp_rack, 0);
    return;
  }
  sim_results->known_opp_rack = *known_opp_rack;
}

BAIResult *sim_results_get_bai_result(const SimResults *sim_results) {
  return sim_results->bai_result;
}

void simmed_play_add_stats_for_ply(SimmedPlay *simmed_play, int ply_index,
                                   const Move *move) {
  const double move_score = equity_to_double(move_get_score(move));
  bool is_bingo = false;
  ply_info_count_t count_type;
  switch (move_get_type(move)) {
  case GAME_EVENT_PASS:
    count_type = PLY_INFO_COUNT_PASS;
    break;
  case GAME_EVENT_EXCHANGE:
    count_type = PLY_INFO_COUNT_EXCHANGE;
    break;
  case GAME_EVENT_TILE_PLACEMENT_MOVE:
    count_type = PLY_INFO_COUNT_TILE_PLACEMENT;
    is_bingo = move_get_tiles_played(move) == RACK_SIZE;
    break;
  default:
    log_fatal(
        "encountered unexpected move type %d when adding stats for ply %d",
        move_get_type(move), ply_index);
    return;
  }
  HeatMap *heat_map = simmed_play_get_heat_map(simmed_play, ply_index);
  cpthread_mutex_lock(&simmed_play->mutex);
  stat_push(simmed_play->ply_infos[ply_index].score_stat, move_score, 1);
  stat_push(simmed_play->ply_infos[ply_index].bingo_stat, (double)(is_bingo),
            1);
  if (heat_map) {
    heat_map_add_move(heat_map, move);
  }
  simmed_play->ply_infos[ply_index].ply_info_counts[count_type]++;
  simmed_play->ply_infos[ply_index].ply_info_counts[PLY_INFO_COUNT_BINGO] +=
      (uint64_t)is_bingo;
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

void sim_results_set_valid_for_current_game_state(SimResults *sim_results,
                                                  bool valid) {
  sim_results->valid_for_current_game_state = valid;
}

bool sim_results_get_valid_for_current_game_state(
    const SimResults *sim_results) {
  return sim_results->valid_for_current_game_state;
}

void sim_results_update_display_simmed_play(const SimResults *sim_results,
                                            const int simmed_play_index) {
  SimmedPlay *simmed_play =
      sim_results_get_simmed_play(sim_results, simmed_play_index);
  SimmedPlay *display_simmed_play =
      sim_results_get_display_simmed_play(sim_results, simmed_play_index);
  const int num_plies = sim_results_get_num_plies(sim_results);
  cpthread_mutex_lock(&simmed_play->mutex);
  simmed_play_copy(display_simmed_play, simmed_play, num_plies);
  cpthread_mutex_unlock(&simmed_play->mutex);
}

int compare_simmed_plays(const void *a, const void *b) {
  const SimmedPlay *play_a = *(SimmedPlay *const *)a;
  const SimmedPlay *play_b = *(SimmedPlay *const *)b;

  const double win_pct_mean_a = stat_get_mean(play_a->win_pct_stat);
  const double win_pct_mean_b = stat_get_mean(play_b->win_pct_stat);
  // Compare the mean values of win_pct_stat
  if (win_pct_mean_a > win_pct_mean_b) {
    return -1;
  }
  if (win_pct_mean_a < win_pct_mean_b) {
    return 1;
  }

  const double equity_mean_a = stat_get_mean(play_a->equity_stat);
  const double equity_mean_b = stat_get_mean(play_b->equity_stat);
  // If win_pct_stat->mean values are equal, compare equity_stat->mean
  if (equity_mean_a > equity_mean_b) {
    return -1;
  }
  if (equity_mean_a < equity_mean_b) {
    return 1;
  }
  return 0;
}

// Returns true if the sim results are ready and the display infos have been
// updated and locked
// Returns false otherwise
bool sim_results_lock_and_sort_display_simmed_plays(SimResults *sim_results) {
  cpthread_mutex_lock(&sim_results->display_mutex);
  if (!sim_results->display_simmed_plays) {
    cpthread_mutex_unlock(&sim_results->display_mutex);
    return false;
  }

  int number_of_simmed_plays = sim_results_get_number_of_plays(sim_results);
  for (int i = 0; i < number_of_simmed_plays; i++) {
    sim_results_update_display_simmed_play(sim_results, i);
  }

  qsort(sim_results->display_simmed_plays, number_of_simmed_plays,
        sizeof(SimmedPlay *), compare_simmed_plays);
  return true;
}

void sim_results_unlock_display_infos(SimResults *sim_results) {
  cpthread_mutex_unlock(&sim_results->display_mutex);
}

SimmedPlay *sim_results_get_display_simmed_play(const SimResults *sim_results,
                                                int play_index) {
  return sim_results->display_simmed_plays[play_index];
}

bool sim_results_plays_are_similar(const SimResults *sim_results,
                                   const int sp1_index, const int sp2_index) {
  SimmedPlay *sp1 = sim_results_get_simmed_play(sim_results, sp1_index);
  if (sp1->similarity_key == 0) {
    sp1->similarity_key = move_get_similarity_key(
        simmed_play_get_move(sp1), sim_results_get_rack(sim_results));
  }
  SimmedPlay *sp2 = sim_results_get_simmed_play(sim_results, sp2_index);
  if (sp2->similarity_key == 0) {
    sp2->similarity_key = move_get_similarity_key(
        simmed_play_get_move(sp2), sim_results_get_rack(sim_results));
  }
  return sp1->similarity_key == sp2->similarity_key;
}
