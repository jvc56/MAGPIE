#include "sim_results.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "../util/lock_profile.h"
#include "bai_result.h"
#include "equity.h"
#include "heat_map.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
#include "win_pct.h"
#include "xoshiro.h"
#include <assert.h>
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

typedef struct PendingRolloutStats {
  uint64_t ordinal;
  int num_completed_plies;
  Move ply_moves[MAX_PLIES];
  Equity initial_spread;
  Equity spread;
  Equity leftover;
  double win_pct;
  double utility;
  bool add_utility;
  bool ready;
} PendingRolloutStats;

struct SimmedPlay {
  Move move;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  // Mean of the per-sample blended utility (see sim_utility_blend). Only
  // meaningful for comparison/display when utility_w_spread > 0.
  Stat *utility_stat;
  uint64_t similarity_key;
  int play_index_by_sort_type;
  XoshiroPRNG *prng;
  int num_alloc_plies;
  PlyInfo *ply_infos;
  // Seed assignment is independent of display/stat updates. Keeping it on a
  // separate mutex prevents a consolidated rollout commit from delaying the
  // next worker's seed generation.
  cpthread_mutex_t seed_mutex;
  cpthread_mutex_t mutex;
  cpthread_cond_t stats_commit_cond;
  uint64_t next_seed_ordinal;
  uint64_t next_stats_ordinal;
  int pending_stats_capacity;
  int pending_stats_count;
  PendingRolloutStats *pending_stats;
  double cutoff;
  // Copied from SimResults at the same points cutoff is refreshed. Nonzero
  // means the BU (blended utility) stat is meaningful for this play.
  double utility_w_spread;
};

struct SimResults {
  int num_simmed_plays;
  int num_alloc_simmed_plays;
  int num_plies;
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
  double cutoff;
  // Utility-blend spread weight the last sim ran with (the SimArgs value,
  // recorded by simulate()). Selects the best-move path in
  // sim_results_get_best_move; see the comment there.
  double utility_w_spread;
  uint64_t num_infer_leaves;
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
                               uint64_t seed, double cutoff, bool use_heat_map,
                               int pending_stats_capacity, const int i) {
  SimmedPlay *simmed_play = malloc_or_die(sizeof(SimmedPlay));
  move_copy(&simmed_play->move, move_list_get_move(move_list, i));
  simmed_play->equity_stat = stat_create(true);
  simmed_play->leftover_stat = stat_create(true);
  simmed_play->win_pct_stat = stat_create(true);
  simmed_play->utility_stat = stat_create(true);
  simmed_play->num_alloc_plies = num_plies;
  simmed_play->ply_infos = malloc_or_die(sizeof(PlyInfo) * num_plies);
  for (int j = 0; j < num_plies; j++) {
    ply_info_init(&simmed_play->ply_infos[j], use_heat_map);
  }
  simmed_play->similarity_key = 0;
  simmed_play->play_index_by_sort_type = i;
  simmed_play->cutoff = cutoff;
  simmed_play->utility_w_spread = 0.0;
  simmed_play->prng = prng_create(seed);
  cpthread_mutex_init(&simmed_play->seed_mutex);
  cpthread_mutex_init(&simmed_play->mutex);
  cpthread_cond_init(&simmed_play->stats_commit_cond);
  simmed_play->next_seed_ordinal = 0;
  simmed_play->next_stats_ordinal = 0;
  simmed_play->pending_stats_capacity = pending_stats_capacity;
  simmed_play->pending_stats_count = 0;
  simmed_play->pending_stats = calloc_or_die(
      (size_t)pending_stats_capacity, sizeof(*simmed_play->pending_stats));
  return simmed_play;
}

SimmedPlay *simmed_play_reset(SimmedPlay *simmed_play,
                              const MoveList *move_list, int new_num_plies,
                              uint64_t seed, double cutoff, bool use_heat_map,
                              int pending_stats_capacity, const int i) {
  move_copy(&simmed_play->move, move_list_get_move(move_list, i));
  stat_reset(simmed_play->equity_stat);
  stat_reset(simmed_play->leftover_stat);
  stat_reset(simmed_play->win_pct_stat);
  stat_reset(simmed_play->utility_stat);
  for (int j = 0; j < simmed_play->num_alloc_plies && j < new_num_plies; j++) {
    ply_info_reset(&simmed_play->ply_infos[j], use_heat_map);
  }
  if (new_num_plies > simmed_play->num_alloc_plies) {
    simmed_play->ply_infos =
        realloc_or_die(simmed_play->ply_infos, sizeof(PlyInfo) * new_num_plies);
    for (int j = simmed_play->num_alloc_plies; j < new_num_plies; j++) {
      ply_info_init(&simmed_play->ply_infos[j], use_heat_map);
    }
    simmed_play->num_alloc_plies = new_num_plies;
  }
  simmed_play->similarity_key = 0;
  simmed_play->play_index_by_sort_type = i;
  simmed_play->cutoff = cutoff;
  prng_seed(simmed_play->prng, seed);
  simmed_play->next_seed_ordinal = 0;
  simmed_play->next_stats_ordinal = 0;
  simmed_play->pending_stats_count = 0;
  if (simmed_play->pending_stats_capacity != pending_stats_capacity) {
    simmed_play->pending_stats = realloc_or_die(
        simmed_play->pending_stats,
        (size_t)pending_stats_capacity * sizeof(*simmed_play->pending_stats));
    simmed_play->pending_stats_capacity = pending_stats_capacity;
  }
  memset(simmed_play->pending_stats, 0,
         (size_t)pending_stats_capacity * sizeof(*simmed_play->pending_stats));
  return simmed_play;
}

SimmedPlay **create_simmed_plays_array(const MoveList *move_list,
                                       const int num_simmed_plays,
                                       const int num_plies, const uint64_t seed,
                                       const double cutoff,
                                       const bool use_heat_map,
                                       const int pending_stats_capacity) {
  SimmedPlay **simmed_plays =
      malloc_or_die((sizeof(SimmedPlay *)) * num_simmed_plays);
  for (int i = 0; i < num_simmed_plays; i++) {
    simmed_plays[i] =
        simmed_play_create(move_list, num_plies, seed, cutoff, use_heat_map,
                           pending_stats_capacity, i);
  }
  return simmed_plays;
}

SimmedPlay **realloc_simmed_plays_array(
    SimmedPlay **simmed_plays, const MoveList *move_list,
    const int old_num_alloc_sps, const int new_num_alloc_sps,
    const int num_plies, const uint64_t seed, const double cutoff,
    const bool use_heat_map, const int pending_stats_capacity) {
  simmed_plays =
      realloc_or_die(simmed_plays, (sizeof(SimmedPlay *)) * new_num_alloc_sps);
  for (int i = old_num_alloc_sps; i < new_num_alloc_sps; i++) {
    simmed_plays[i] =
        simmed_play_create(move_list, num_plies, seed, cutoff, use_heat_map,
                           pending_stats_capacity, i);
  }
  return simmed_plays;
}

void sim_results_create_simmed_plays(SimResults *sim_results,
                                     const MoveList *move_list, int num_plies,
                                     uint64_t seed, bool use_heat_map,
                                     int num_threads) {
  const int num_simmed_plays = move_list_get_count(move_list);
  sim_results->num_simmed_plays = num_simmed_plays;
  sim_results->num_alloc_simmed_plays = num_simmed_plays;
  sim_results->num_plies = num_plies;
  // FIXME: ensure heatmaps are off for sim autoplay
  sim_results->simmed_plays =
      create_simmed_plays_array(move_list, num_simmed_plays, num_plies, seed,
                                sim_results->cutoff, use_heat_map, num_threads);
  // FIXME: don't create display simmed plays for sim autoplay
  sim_results->display_simmed_plays = create_simmed_plays_array(
      move_list, num_simmed_plays, num_plies, 0, 0, false, 1);
}

void sim_results_simmed_plays_reset(SimResults *sim_results,
                                    const MoveList *move_list, int num_plies,
                                    uint64_t seed, bool use_heat_map,
                                    int num_threads) {
  const int new_num_sps = move_list_get_count(move_list);
  for (int i = 0; i < sim_results->num_alloc_simmed_plays && i < new_num_sps;
       i++) {
    simmed_play_reset(sim_results->simmed_plays[i], move_list, num_plies, seed,
                      sim_results->cutoff, use_heat_map, num_threads, i);
    simmed_play_reset(sim_results->display_simmed_plays[i], move_list,
                      num_plies, 0, 0, false, 1, i);
  }
  sim_results->num_plies = num_plies;
  sim_results->num_simmed_plays = new_num_sps;
  if (new_num_sps > sim_results->num_alloc_simmed_plays) {
    sim_results->simmed_plays = realloc_simmed_plays_array(
        sim_results->simmed_plays, move_list,
        sim_results->num_alloc_simmed_plays, new_num_sps, num_plies, seed,
        sim_results->cutoff, use_heat_map, num_threads);
    sim_results->display_simmed_plays =
        realloc_simmed_plays_array(sim_results->display_simmed_plays, move_list,
                                   sim_results->num_alloc_simmed_plays,
                                   new_num_sps, num_plies, 0, 0, false, 1);
    sim_results->num_alloc_simmed_plays = new_num_sps;
  }
}

// Does not copy the following fields:
// - PRNG
// - mutex
// - heat_map
// - num_alloc_plies
// - cutoff
// - utility_w_spread
void simmed_play_copy(SimmedPlay *dst, const SimmedPlay *src,
                      const int num_plies) {
  move_copy(&dst->move, &src->move);
  stat_copy(dst->equity_stat, src->equity_stat);
  stat_copy(dst->leftover_stat, src->leftover_stat);
  stat_copy(dst->win_pct_stat, src->win_pct_stat);
  stat_copy(dst->utility_stat, src->utility_stat);
  dst->similarity_key = src->similarity_key;
  dst->play_index_by_sort_type = src->play_index_by_sort_type;
  for (int i = 0; i < num_plies; i++) {
    stat_copy(dst->ply_infos[i].score_stat, src->ply_infos[i].score_stat);
    stat_copy(dst->ply_infos[i].bingo_stat, src->ply_infos[i].bingo_stat);
    memcpy(dst->ply_infos[i].ply_info_counts, src->ply_infos[i].ply_info_counts,
           sizeof(dst->ply_infos[i].ply_info_counts));
  }
}

void simmed_plays_destroy(SimmedPlay **simmed_plays, int num_alloc_sps) {
  if (!simmed_plays) {
    return;
  }
  for (int i = 0; i < num_alloc_sps; i++) {
    for (int j = 0; j < simmed_plays[i]->num_alloc_plies; j++) {
      stat_destroy(simmed_plays[i]->ply_infos[j].bingo_stat);
      stat_destroy(simmed_plays[i]->ply_infos[j].score_stat);
      heat_map_destroy(simmed_plays[i]->ply_infos[j].heat_map);
    }
    free(simmed_plays[i]->ply_infos);
    stat_destroy(simmed_plays[i]->equity_stat);
    stat_destroy(simmed_plays[i]->leftover_stat);
    stat_destroy(simmed_plays[i]->win_pct_stat);
    stat_destroy(simmed_plays[i]->utility_stat);
    prng_destroy(simmed_plays[i]->prng);
    free(simmed_plays[i]->pending_stats);
    free(simmed_plays[i]);
  }
  free(simmed_plays);
}

void sim_results_destroy(SimResults *sim_results) {
  if (!sim_results) {
    return;
  }
  simmed_plays_destroy(sim_results->simmed_plays,
                       sim_results->num_alloc_simmed_plays);
  simmed_plays_destroy(sim_results->display_simmed_plays,
                       sim_results->num_alloc_simmed_plays);
  bai_result_destroy(sim_results->bai_result);
  free(sim_results);
}

void sim_results_lock_simmed_plays(SimResults *sim_results) {
  lock_profile_mutex_lock(&sim_results->simmed_plays_mutex,
                          LOCK_PROFILE_SITE_DISPLAY);
}

void sim_results_unlock_simmed_plays(SimResults *sim_results) {
  lock_profile_mutex_unlock(&sim_results->simmed_plays_mutex,
                            LOCK_PROFILE_SITE_DISPLAY);
}

void sim_results_reset(const MoveList *move_list, SimResults *sim_results,
                       int num_plies, uint64_t seed, bool use_heat_map,
                       int num_threads) {
  assert(num_threads > 0);
  lock_profile_mutex_lock(&sim_results->display_mutex,
                          LOCK_PROFILE_SITE_DISPLAY);
  if (!sim_results->simmed_plays) {
    sim_results_create_simmed_plays(sim_results, move_list, num_plies, seed,
                                    use_heat_map, num_threads);
  } else {
    sim_results_simmed_plays_reset(sim_results, move_list, num_plies, seed,
                                   use_heat_map, num_threads);
  }
  atomic_init(&sim_results->node_count, 0);
  atomic_init(&sim_results->iteration_count, 0);
  sim_results->valid_for_current_game_state = false;
  lock_profile_mutex_unlock(&sim_results->display_mutex,
                            LOCK_PROFILE_SITE_DISPLAY);
}

SimResults *sim_results_create(const double cutoff) {
  SimResults *sim_results = malloc_or_die(sizeof(SimResults));
  sim_results->num_simmed_plays = 0;
  sim_results->num_alloc_simmed_plays = 0;
  sim_results->num_plies = 0;
  atomic_init(&sim_results->node_count, 0);
  atomic_init(&sim_results->iteration_count, 0);
  cpthread_mutex_init(&sim_results->simmed_plays_mutex);
  cpthread_mutex_init(&sim_results->display_mutex);
  sim_results->simmed_plays = NULL;
  sim_results->display_simmed_plays = NULL;
  sim_results->bai_result = bai_result_create();
  sim_results->valid_for_current_game_state = false;
  sim_results->cutoff = cutoff;
  sim_results->utility_w_spread = 0.0;
  sim_results->num_infer_leaves = 0;
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

const Stat *simmed_play_get_leftover_stat(const SimmedPlay *simmed_play) {
  return simmed_play->leftover_stat;
}

const Stat *simmed_play_get_win_pct_stat(const SimmedPlay *simmed_play) {
  return simmed_play->win_pct_stat;
}

const Stat *simmed_play_get_utility_stat(const SimmedPlay *simmed_play) {
  return simmed_play->utility_stat;
}

const HeatMap *simmed_play_get_heat_map_const(const SimmedPlay *simmed_play,
                                              int ply_index) {
  return simmed_play->ply_infos[ply_index].heat_map;
}

bool simmed_play_get_utility_w_spread_is_set(const SimmedPlay *simmed_play) {
  return simmed_play->utility_w_spread > 0.0;
}

int simmed_play_get_play_index_by_sort_type(const SimmedPlay *simmed_play) {
  return simmed_play->play_index_by_sort_type;
}

// Returns the current seed and updates the seed using prng_next
uint64_t simmed_play_get_seed(SimmedPlay *simmed_play,
                              uint64_t *arm_sample_ordinal) {
  uint64_t seed;
  lock_profile_mutex_lock(&simmed_play->seed_mutex, LOCK_PROFILE_SITE_SIM_SEED);
  *arm_sample_ordinal = simmed_play->next_seed_ordinal++;
  seed = prng_next(simmed_play->prng);
  lock_profile_mutex_unlock(&simmed_play->seed_mutex,
                            LOCK_PROFILE_SITE_SIM_SEED);
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

double sim_results_get_cutoff(const SimResults *sim_results) {
  return sim_results->cutoff;
}

void sim_results_set_cutoff(SimResults *sim_results, double cutoff) {
  sim_results->cutoff = cutoff;
}

void sim_results_set_utility_w_spread(SimResults *sim_results,
                                      double utility_w_spread) {
  sim_results->utility_w_spread = utility_w_spread;
  for (int i = 0; i < sim_results->num_simmed_plays; i++) {
    sim_results->simmed_plays[i]->utility_w_spread = utility_w_spread;
  }
}

double sim_results_get_utility_w_spread(const SimResults *sim_results) {
  return sim_results->utility_w_spread;
}

uint64_t sim_results_get_num_infer_leaves(const SimResults *sim_results) {
  return sim_results->num_infer_leaves;
}

void sim_results_set_num_infer_leaves(SimResults *sim_results,
                                      uint64_t num_infer_leaves) {
  sim_results->num_infer_leaves = num_infer_leaves;
}

static void simmed_play_add_stats_for_ply_unlocked(SimmedPlay *simmed_play,
                                                   int ply_index,
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
  stat_push(simmed_play->ply_infos[ply_index].score_stat, move_score, 1);
  stat_push(simmed_play->ply_infos[ply_index].bingo_stat, (double)(is_bingo),
            1);
  if (heat_map) {
    heat_map_add_move(heat_map, move);
  }
  simmed_play->ply_infos[ply_index].ply_info_counts[count_type]++;
  simmed_play->ply_infos[ply_index].ply_info_counts[PLY_INFO_COUNT_BINGO] +=
      (uint64_t)is_bingo;
}

int round_to_nearest_int(double a) {
  return (int)(a + 0.5 - (a < 0)); // truncated to 55
}

double simmed_play_calculate_win_pct(const WinPct *wp, Equity spread,
                                     Equity leftover,
                                     game_end_reason_t game_end_reason,
                                     int game_unseen_tiles,
                                     bool plies_are_odd) {
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
  return wpct;
}

static void
simmed_play_apply_rollout_stats_unlocked(SimmedPlay *simmed_play,
                                         const PendingRolloutStats *pending) {
  for (int ply = 0; ply < pending->num_completed_plies; ply++) {
    simmed_play_add_stats_for_ply_unlocked(simmed_play, ply,
                                           &pending->ply_moves[ply]);
  }
  stat_push(simmed_play->equity_stat,
            equity_to_double(pending->spread - pending->initial_spread +
                             pending->leftover),
            1);
  stat_push(simmed_play->leftover_stat, equity_to_double(pending->leftover), 1);
  stat_push(simmed_play->win_pct_stat, pending->win_pct, 1);
  if (pending->add_utility) {
    stat_push(simmed_play->utility_stat, pending->utility, 1);
  }
}

static PendingRolloutStats *
simmed_play_find_pending_ordinal(SimmedPlay *simmed_play, uint64_t ordinal) {
  for (int i = 0; i < simmed_play->pending_stats_capacity; i++) {
    PendingRolloutStats *pending = &simmed_play->pending_stats[i];
    if (pending->ready && pending->ordinal == ordinal) {
      return pending;
    }
  }
  return NULL;
}

static PendingRolloutStats *
simmed_play_find_empty_pending_slot(SimmedPlay *simmed_play) {
  for (int i = 0; i < simmed_play->pending_stats_capacity; i++) {
    PendingRolloutStats *pending = &simmed_play->pending_stats[i];
    if (!pending->ready) {
      return pending;
    }
  }
  return NULL;
}

void simmed_play_commit_rollout_stats(SimmedPlay *simmed_play,
                                      uint64_t arm_sample_ordinal,
                                      int num_completed_plies,
                                      const Move *ply_moves,
                                      Equity initial_spread, Equity spread,
                                      Equity leftover, double win_pct,
                                      double utility, bool add_utility) {
  // A rollout used to acquire this mutex once per completed ply and once each
  // for equity, win percentage, and optional utility. Commit the already
  // computed delta as one transaction so display snapshots remain coherent
  // while avoiding repeated sleep/wakeup cycles on the same arm.
  PendingRolloutStats completed = {
      .ordinal = arm_sample_ordinal,
      .num_completed_plies = num_completed_plies,
      .initial_spread = initial_spread,
      .spread = spread,
      .leftover = leftover,
      .win_pct = win_pct,
      .utility = utility,
      .add_utility = add_utility,
      .ready = true,
  };
  for (int ply = 0; ply < num_completed_plies; ply++) {
    move_copy(&completed.ply_moves[ply], &ply_moves[ply]);
  }

  lock_profile_mutex_lock(&simmed_play->mutex, LOCK_PROFILE_SITE_SIM_PLY_STATS);
  // Seeds give each arm a stable logical order, but workers can finish in a
  // different physical order. Publish in seed order so floating Stat updates
  // remain bit-identical across thread counts. At most num_threads - 1 later
  // rollouts can be pending; once that bound is reached, later finishers wait
  // and leave the missing ordinal's worker free to make progress.
  while (arm_sample_ordinal != simmed_play->next_stats_ordinal &&
         simmed_play->pending_stats_count >=
             simmed_play->pending_stats_capacity - 1) {
    lock_profile_cond_wait(&simmed_play->stats_commit_cond, &simmed_play->mutex,
                           LOCK_PROFILE_SITE_SIM_PLY_STATS);
  }

  if (arm_sample_ordinal == simmed_play->next_stats_ordinal) {
    simmed_play_apply_rollout_stats_unlocked(simmed_play, &completed);
    simmed_play->next_stats_ordinal++;
    while (true) {
      PendingRolloutStats *pending = simmed_play_find_pending_ordinal(
          simmed_play, simmed_play->next_stats_ordinal);
      if (pending == NULL) {
        break;
      }
      simmed_play_apply_rollout_stats_unlocked(simmed_play, pending);
      pending->ready = false;
      simmed_play->pending_stats_count--;
      simmed_play->next_stats_ordinal++;
    }
    cpthread_cond_broadcast(&simmed_play->stats_commit_cond);
  } else {
    PendingRolloutStats *pending =
        simmed_play_find_empty_pending_slot(simmed_play);
    if (pending == NULL) {
      log_fatal("no free pending sim stats slot");
    }
    *pending = completed;
    simmed_play->pending_stats_count++;
  }
  lock_profile_mutex_unlock(&simmed_play->mutex,
                            LOCK_PROFILE_SITE_SIM_PLY_STATS);
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
  lock_profile_mutex_lock(&simmed_play->mutex, LOCK_PROFILE_SITE_DISPLAY);
  simmed_play_copy(display_simmed_play, simmed_play, num_plies);
  lock_profile_mutex_unlock(&simmed_play->mutex, LOCK_PROFILE_SITE_DISPLAY);
}

// When simming stuck tile endgames, a pass will sim equivalently to the
// best greedy sequence because the sim assumes that after passing the
// greedy sequence will begin, so a pass now versus later makes no
// difference. However, the other player will also do this and pass as well.
// The simmed endgame does not take into account the six pass rule, and so
// the game will end in a six pass. To avoid this, if a pass is tied with
// the best play, prefer to not pass.
static int compare_simmed_plays_pass_tiebreak(const SimmedPlay *play_a,
                                              const SimmedPlay *play_b) {
  const bool play_a_is_pass =
      move_get_type(simmed_play_get_move(play_a)) == GAME_EVENT_PASS;
  const bool play_b_is_pass =
      move_get_type(simmed_play_get_move(play_b)) == GAME_EVENT_PASS;
  if (!play_a_is_pass && play_b_is_pass) {
    return -1;
  }
  if (play_a_is_pass && !play_b_is_pass) {
    return 1;
  }
  return 0;
}

int compare_simmed_plays(const void *a, const void *b) {
  const SimmedPlay *play_a = *(SimmedPlay *const *)a;
  const SimmedPlay *play_b = *(SimmedPlay *const *)b;

  // With a nonzero spread weight, BAI selects the arm with the highest mean
  // blended utility (BU); rank the display the same way so the sort order
  // matches sim_results_get_best_move's choice.
  if (play_a->utility_w_spread > 0.0) {
    const double utility_mean_a = stat_get_mean(play_a->utility_stat);
    const double utility_mean_b = stat_get_mean(play_b->utility_stat);
    if (utility_mean_a > utility_mean_b) {
      return -1;
    }
    if (utility_mean_a < utility_mean_b) {
      return 1;
    }
    return compare_simmed_plays_pass_tiebreak(play_a, play_b);
  }

  const double win_pct_mean_a = stat_get_mean(play_a->win_pct_stat);
  const double win_pct_mean_b = stat_get_mean(play_b->win_pct_stat);
  const double cutoff = play_a->cutoff;
  if (are_win_pcts_within_cutoff_or_equal(win_pct_mean_a, win_pct_mean_b,
                                          cutoff)) {
    const double equity_mean_a = stat_get_mean(play_a->equity_stat);
    const double equity_mean_b = stat_get_mean(play_b->equity_stat);
    // Compare by equity_stat->mean
    if (equity_mean_a > equity_mean_b) {
      return -1;
    }
    if (equity_mean_a < equity_mean_b) {
      return 1;
    }
    return compare_simmed_plays_pass_tiebreak(play_a, play_b);
  }

  if (win_pct_mean_a > win_pct_mean_b) {
    return -1;
  }
  if (win_pct_mean_a < win_pct_mean_b) {
    return 1;
  }
  return 0;
}

// Returns true if the sim results are ready and the display infos have been
// updated and locked
// Returns false otherwise
bool sim_results_lock_and_sort_display_simmed_plays(SimResults *sim_results) {
  lock_profile_mutex_lock(&sim_results->display_mutex,
                          LOCK_PROFILE_SITE_DISPLAY);
  if (!sim_results->display_simmed_plays) {
    lock_profile_mutex_unlock(&sim_results->display_mutex,
                              LOCK_PROFILE_SITE_DISPLAY);
    return false;
  }

  int number_of_simmed_plays = sim_results_get_number_of_plays(sim_results);
  for (int i = 0; i < number_of_simmed_plays; i++) {
    sim_results_update_display_simmed_play(sim_results, i);
    sim_results->display_simmed_plays[i]->cutoff = sim_results->cutoff;
    sim_results->display_simmed_plays[i]->utility_w_spread =
        sim_results->utility_w_spread;
  }

  qsort(sim_results->display_simmed_plays, number_of_simmed_plays,
        sizeof(SimmedPlay *), compare_simmed_plays);
  return true;
}

void sim_results_unlock_display_infos(SimResults *sim_results) {
  lock_profile_mutex_unlock(&sim_results->display_mutex,
                            LOCK_PROFILE_SITE_DISPLAY);
}

SimmedPlay *sim_results_get_display_simmed_play(const SimResults *sim_results,
                                                int play_index) {
  return sim_results->display_simmed_plays[play_index];
}

bool sim_results_simmed_plays_are_similar_internal(
    const SimResults *sim_results, SimmedPlay *sp1, SimmedPlay *sp2) {
  if (sp1->similarity_key == 0) {
    sp1->similarity_key = move_get_similarity_key(
        simmed_play_get_move(sp1), sim_results_get_rack(sim_results));
  }
  if (sp2->similarity_key == 0) {
    sp2->similarity_key = move_get_similarity_key(
        simmed_play_get_move(sp2), sim_results_get_rack(sim_results));
  }
  return sp1->similarity_key == sp2->similarity_key;
}

bool sim_results_display_plays_are_similar(const SimResults *sim_results,
                                           const int sp1_index,
                                           const int sp2_index) {
  SimmedPlay *sp1 = sim_results_get_display_simmed_play(sim_results, sp1_index);
  SimmedPlay *sp2 = sim_results_get_display_simmed_play(sim_results, sp2_index);
  return sim_results_simmed_plays_are_similar_internal(sim_results, sp1, sp2);
}

bool sim_results_plays_are_similar(const SimResults *sim_results,
                                   const int sp1_index, const int sp2_index) {
  SimmedPlay *sp1 = sim_results_get_simmed_play(sim_results, sp1_index);
  SimmedPlay *sp2 = sim_results_get_simmed_play(sim_results, sp2_index);
  return sim_results_simmed_plays_are_similar_internal(sim_results, sp1, sp2);
}

// Index of the best play under compare_simmed_plays, which is also the order
// the display is sorted in. With a nonzero spread weight that comparator ranks
// by mean blended utility -- the same quantity BAI maximizes when it picks its
// best arm -- so there is no need to consult the BAI result separately. With a
// ZERO spread weight the sample utility is the raw 0/0.5/1 win outcome, which
// loses its gradient whenever win% saturates (decided games: every arm's mean
// is ~0 or ~1) or ties exactly; the comparator's win%-within-cutoff tiebreak on
// mean equity recovers spread at no win% cost.
// Not thread safe, assumes the sim is finished.
int sim_results_get_best_move_index(const SimResults *sim_results) {
  const int num_simmed_plays = sim_results_get_number_of_plays(sim_results);
  if (num_simmed_plays == 0) {
    return -1;
  }
  const SimmedPlay *current_best_simmed_play =
      sim_results_get_simmed_play(sim_results, 0);
  int best_play_idx = 0;
  for (int play_idx = 1; play_idx < num_simmed_plays; play_idx++) {
    const SimmedPlay *sp = sim_results_get_simmed_play(sim_results, play_idx);
    if (compare_simmed_plays(&sp, &current_best_simmed_play) < 0) {
      current_best_simmed_play = sp;
      best_play_idx = play_idx;
    }
  }
  return best_play_idx;
}

// Not thread safe, assumes the sim is finished.
const Move *sim_results_get_best_move(const SimResults *sim_results) {
  const int best_play_idx = sim_results_get_best_move_index(sim_results);
  if (best_play_idx < 0) {
    return NULL;
  }
  return simmed_play_get_move(
      sim_results_get_simmed_play(sim_results, best_play_idx));
}

// Mean utility (win%+spread blend in [0, 1]) of the sim's best play, using the
// same best-play selection as sim_results_get_best_move. Not thread safe,
// assumes the sim is finished. Returns 0 if there are no plays.
double sim_results_get_best_move_utility(const SimResults *sim_results) {
  const int best_play_idx = sim_results_get_best_move_index(sim_results);
  if (best_play_idx < 0) {
    return 0.0;
  }
  const SimmedPlay *best_play =
      sim_results_get_simmed_play(sim_results, best_play_idx);
  // utility_stat is only recorded on the utility path (the hot pure-win% path
  // skips it). With a zero spread weight the utility IS the win% -- read it
  // from win_pct_stat rather than the empty utility_stat, which would report 0.
  if (sim_results->utility_w_spread == 0.0) {
    return stat_get_mean(simmed_play_get_win_pct_stat(best_play));
  }
  return stat_get_mean(simmed_play_get_utility_stat(best_play));
}
