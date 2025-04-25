#include "random_variable.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "../ent/game.h"
#include "../ent/player.h"
#include "../ent/sim_results.h"
#include "../ent/xoshiro.h"

#include "bai_logger.h"
#include "gameplay.h"

#include "../str/sim_string.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

#define SIMILARITY_EPSILON 1e-6

typedef double (*rvs_sample_func_t)(RandomVariables *, const uint64_t,
                                    const int, const uint64_t, BAILogger *);
typedef bool (*rvs_similar_func_t)(RandomVariables *, const int, const int);
typedef bool (*rvs_is_epigon_func_t)(const RandomVariables *, const int);
typedef void (*rvs_destroy_data_func_t)(RandomVariables *);

struct RandomVariables {
  int num_rvs;
  atomic_uint_fast64_t total_samples;
  rvs_sample_func_t sample_func;
  rvs_similar_func_t similar_func;
  rvs_is_epigon_func_t is_epigon_func;
  rvs_destroy_data_func_t destroy_data_func;
  void *data;
};

double uniform_sample(XoshiroPRNG *prng) {
  return (double)prng_next(prng) / ((double)UINT64_MAX);
}

typedef struct RVUniform {
  XoshiroPRNG *xoshiro_prng;
} RVUniform;

double rv_uniform_sample(RandomVariables *rvs,
                         const uint64_t __attribute__((unused)) k,
                         const int __attribute__((unused)) thread_index,
                         const uint64_t __attribute__((unused)) sample_count,
                         BAILogger __attribute__((unused)) * bai_logger) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  return uniform_sample(rv_uniform->xoshiro_prng);
}

bool rv_uniform_mark_as_epigon_if_similar(RandomVariables
                                              __attribute__((unused)) *
                                              rvs,
                                          const int __attribute__((unused)) i,
                                          const int __attribute__((unused)) j) {
  return false;
}

bool rv_uniform_is_epigon(const RandomVariables __attribute__((unused)) * rvs,
                          const int __attribute__((unused)) i) {
  return false;
}

void rv_uniform_destroy(RandomVariables *rvs) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  prng_destroy(rv_uniform->xoshiro_prng);
  free(rv_uniform);
}

void rv_uniform_create(RandomVariables *rvs, const uint64_t seed) {
  rvs->sample_func = rv_uniform_sample;
  rvs->similar_func = rv_uniform_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_uniform_is_epigon;
  rvs->destroy_data_func = rv_uniform_destroy;
  RVUniform *rv_uniform = malloc_or_die(sizeof(RVUniform));
  rv_uniform->xoshiro_prng = prng_create(seed);
  rvs->data = rv_uniform;
}

typedef struct RVUniformPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
} RVUniformPredetermined;

double rv_uniform_predetermined_sample(
    RandomVariables *rvs, const uint64_t __attribute__((unused)) k,
    const int __attribute__((unused)) thread_index,
    const uint64_t __attribute__((unused)) sample_count,
    BAILogger __attribute__((unused)) * bai_logger) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  if (rv_uniform_predetermined->index >=
      rv_uniform_predetermined->num_samples) {
    log_fatal("ran out of uniform predetermined samples\n");
  }
  const int index = rv_uniform_predetermined->index++;
  const double result = rv_uniform_predetermined->samples[index];
  return result;
}

bool rv_uniform_predetermined_mark_as_epigon_if_similar(
    RandomVariables __attribute__((unused)) * rvs,
    const int __attribute__((unused)) i, const int __attribute__((unused)) j) {
  return false;
}

bool rv_uniform_predetermined_is_epigon(const RandomVariables
                                            __attribute__((unused)) *
                                            rvs,
                                        const int __attribute__((unused)) i) {
  return false;
}

void rv_uniform_predetermined_destroy(RandomVariables *rvs) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  free(rv_uniform_predetermined->samples);
  free(rv_uniform_predetermined);
}

void rv_uniform_predetermined_create(RandomVariables *rvs,
                                     const double *samples,
                                     const uint64_t num_samples) {
  rvs->sample_func = rv_uniform_predetermined_sample;
  rvs->similar_func = rv_uniform_predetermined_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_uniform_predetermined_is_epigon;
  rvs->destroy_data_func = rv_uniform_predetermined_destroy;
  RVUniformPredetermined *rv_uniform_predetermined =
      malloc_or_die(sizeof(RVUniformPredetermined));
  rv_uniform_predetermined->num_samples = num_samples;
  rv_uniform_predetermined->index = 0;
  rv_uniform_predetermined->samples =
      malloc_or_die(rv_uniform_predetermined->num_samples * sizeof(double));
  memory_copy(rv_uniform_predetermined->samples, samples,
              rv_uniform_predetermined->num_samples * sizeof(double));
  rvs->data = rv_uniform_predetermined;
}

typedef struct RVNormal {
  XoshiroPRNG *xoshiro_prng;
  double *means_and_vars;
  bool *is_epigon;
} RVNormal;

double rv_normal_sample(RandomVariables *rvs, const uint64_t k,
                        const int __attribute__((unused)) thread_index,
                        const uint64_t __attribute__((unused)) sample_count,
                        BAILogger __attribute__((unused)) * bai_logger) {
  // Implements the Box-Muller transform
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  double u, v, s;
  s = 2.0;
  while (s >= 1.0 || s == 0.0) {
    u = 2.0 * uniform_sample(rv_normal->xoshiro_prng) - 1.0;
    v = 2.0 * uniform_sample(rv_normal->xoshiro_prng) - 1.0;
    s = u * u + v * v;
  }
  s = sqrt(-2.0 * log(s) / s);
  return rv_normal->means_and_vars[k * 2] +
         rv_normal->means_and_vars[k * 2 + 1] * u * s;
}

bool rv_normal_mark_as_epigon_if_similar(RandomVariables *rvs, const int leader,
                                         const int i) {
  if (leader == i) {
    return false;
  }
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  rv_normal->is_epigon[i] =
      fabs(rv_normal->means_and_vars[leader * 2] -
           rv_normal->means_and_vars[i * 2]) < SIMILARITY_EPSILON &&
      fabs(rv_normal->means_and_vars[leader * 2 + 1] -
           rv_normal->means_and_vars[i * 2 + 1]) < SIMILARITY_EPSILON;
  return rv_normal->is_epigon[i];
}

bool rv_normal_is_epigon(const RandomVariables *rvs, const int i) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  return rv_normal->is_epigon[i];
}

void rv_normal_destroy(RandomVariables *rvs) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  prng_destroy(rv_normal->xoshiro_prng);
  free(rv_normal->means_and_vars);
  free(rv_normal->is_epigon);
  free(rv_normal);
}

void rv_normal_create(RandomVariables *rvs, const uint64_t seed,
                      const double *means_and_vars) {
  rvs->sample_func = rv_normal_sample;
  rvs->similar_func = rv_normal_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_normal_is_epigon;
  rvs->destroy_data_func = rv_normal_destroy;
  RVNormal *rv_normal = malloc_or_die(sizeof(RVNormal));
  rv_normal->xoshiro_prng = prng_create(seed);
  rv_normal->means_and_vars = malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memory_copy(rv_normal->means_and_vars, means_and_vars,
              rvs->num_rvs * 2 * sizeof(double));
  rv_normal->is_epigon = calloc_or_die(rvs->num_rvs, sizeof(bool));
  rvs->data = rv_normal;
}

typedef struct RVNormalPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
  double *means_and_vars;
  bool *is_epigon;
} RVNormalPredetermined;

double rv_normal_predetermined_sample(RandomVariables *rvs, const uint64_t k,
                                      const int
                                      __attribute__((unused)) thread_index,
                                      const uint64_t
                                      __attribute__((unused)) sample_count,
                                      BAILogger *bai_logger) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  if (rv_normal_predetermined->index >= rv_normal_predetermined->num_samples) {
    log_fatal("ran out of normal predetermined samples\n");
  }
  const double mean = rv_normal_predetermined->means_and_vars[k * 2];
  const double sigma2 = rv_normal_predetermined->means_and_vars[k * 2 + 1];
  const int index = rv_normal_predetermined->index++;
  const double sample = rv_normal_predetermined->samples[index];
  const double result = mean + sqrt(sigma2) * sample;
  bai_logger_log_title(bai_logger, "DETERMINISTIC_SAMPLE");
  bai_logger_log_int(bai_logger, "index", index + 1);
  bai_logger_log_int(bai_logger, "arm", k + 1);
  bai_logger_log_double(bai_logger, "s", result);
  bai_logger_log_double(bai_logger, "u", mean);
  bai_logger_log_double(bai_logger, "sigma2", sigma2);
  bai_logger_log_double(bai_logger, "samp", sample);
  bai_logger_flush(bai_logger);
  return result;
}

bool rv_normal_predetermined_mark_as_epigon_if_similar(RandomVariables *rvs,
                                                       const int leader,
                                                       const int i) {
  if (leader == i) {
    return false;
  }
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  rv_normal_predetermined->is_epigon[i] =
      fabs(rv_normal_predetermined->means_and_vars[leader * 2] -
           rv_normal_predetermined->means_and_vars[i * 2]) <
          SIMILARITY_EPSILON &&
      fabs(rv_normal_predetermined->means_and_vars[leader * 2 + 1] -
           rv_normal_predetermined->means_and_vars[i * 2 + 1]) <
          SIMILARITY_EPSILON;
  return rv_normal_predetermined->is_epigon[i];
}

bool rv_normal_predetermined_is_epigon(const RandomVariables *rvs,
                                       const int i) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  return rv_normal_predetermined->is_epigon[i];
}

void rv_normal_predetermined_destroy(RandomVariables *rvs) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  free(rv_normal_predetermined->samples);
  free(rv_normal_predetermined->means_and_vars);
  free(rv_normal_predetermined->is_epigon);
  free(rv_normal_predetermined);
}

void rv_normal_predetermined_create(RandomVariables *rvs, const double *samples,
                                    const uint64_t num_samples,
                                    const double *means_and_vars) {
  rvs->sample_func = rv_normal_predetermined_sample;
  rvs->similar_func = rv_normal_predetermined_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_normal_predetermined_is_epigon;
  rvs->destroy_data_func = rv_normal_predetermined_destroy;
  RVNormalPredetermined *rv_normal_predetermined =
      malloc_or_die(sizeof(RVNormalPredetermined));
  rv_normal_predetermined->num_samples = num_samples;
  rv_normal_predetermined->index = 0;
  rv_normal_predetermined->samples =
      malloc_or_die(rv_normal_predetermined->num_samples * sizeof(double));
  memory_copy(rv_normal_predetermined->samples, samples,
              rv_normal_predetermined->num_samples * sizeof(double));
  rv_normal_predetermined->means_and_vars =
      malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memory_copy(rv_normal_predetermined->means_and_vars, means_and_vars,
              rvs->num_rvs * 2 * sizeof(double));
  rv_normal_predetermined->is_epigon =
      calloc_or_die(rvs->num_rvs, sizeof(bool));
  rvs->data = rv_normal_predetermined;
}

typedef struct SimmerWorker {
  Game *game;
  MoveList *move_list;
} SimmerWorker;

typedef struct Simmer {
  Equity initial_spread;
  int initial_player;
  Rack *known_opp_rack;
  SimmerWorker **workers;
  // Owned by the caller
  const WinPct *win_pcts;
  ThreadControl *thread_control;
  SimResults *sim_results;
} Simmer;

SimmerWorker *simmer_create_worker(const Game *game) {
  SimmerWorker *simmer_worker = malloc_or_die(sizeof(SimmerWorker));
  simmer_worker->game = game_duplicate(game);
  game_set_backup_mode(simmer_worker->game, BACKUP_MODE_SIMULATION);
  simmer_worker->move_list = move_list_create(1);
  return simmer_worker;
}

void simmer_worker_destroy(SimmerWorker *simmer_worker) {
  if (!simmer_worker) {
    return;
  }
  game_destroy(simmer_worker->game);
  move_list_destroy(simmer_worker->move_list);
  free(simmer_worker);
}

bool simmer_plays_are_similar(const Simmer *simmer,
                              const int current_best_play_index,
                              const int other_play_index) {
  SimResults *sim_results = simmer->sim_results;
  SimmedPlay *m1 =
      sim_results_get_simmed_play(sim_results, current_best_play_index);
  SimmedPlay *m2 = sim_results_get_simmed_play(sim_results, other_play_index);

  Move *m1_move = simmed_play_get_move(m1);
  Move *m2_move = simmed_play_get_move(m2);

  return moves_are_similar(m1_move, m2_move);
}

double rv_sim_sample(RandomVariables *rvs, const uint64_t play_index,
                     const int thread_index, const uint64_t sample_count,
                     BAILogger __attribute__((unused)) * bai_logger) {
  Simmer *simmer = (Simmer *)rvs->data;
  SimResults *sim_results = simmer->sim_results;
  SimmedPlay *simmed_play =
      sim_results_get_simmed_play(sim_results, play_index);
  if (simmed_play_get_is_epigon(simmed_play)) {
    return INFINITY;
  }
  SimmerWorker *simmer_worker = simmer->workers[thread_index];
  Game *game = simmer_worker->game;
  MoveList *move_list = simmer_worker->move_list;
  int plies = sim_results_get_max_plies(sim_results);

  // This will shuffle the bag, so there is no need
  // to call bag_shuffle explicitly.
  game_seed(game, simmed_play_get_seed(simmed_play));

  int player_off_turn_index = 1 - game_get_player_on_turn_index(game);
  // set random rack for opponent (throw in rack, bag_shuffle, draw new tiles).
  set_random_rack(game, player_off_turn_index, simmer->known_opp_rack);

  Equity leftover = 0;
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  // play move
  play_move(simmed_play_get_move(simmed_play), game, NULL, NULL);
  sim_results_increment_node_count(sim_results);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  // further plies will NOT be backed up.
  Rack spare_rack;
  for (int ply = 0; ply < plies; ply++) {
    int player_on_turn_index = game_get_player_on_turn_index(game);
    Player *player_on_turn = game_get_player(game, player_on_turn_index);

    if (game_over(game)) {
      break;
    }

    const Move *best_play = get_top_equity_move(game, thread_index, move_list);
    rack_copy(&spare_rack, player_get_rack(player_on_turn));

    play_move(best_play, game, NULL, NULL);
    sim_results_increment_node_count(sim_results);
    if (ply == plies - 2 || ply == plies - 1) {
      Equity this_leftover = get_leave_value_for_move(
          player_get_klv(player_on_turn), best_play, &spare_rack);
      if (player_on_turn_index == simmer->initial_player) {
        leftover += this_leftover;
      } else {
        leftover -= this_leftover;
      }
    }
    simmed_play_add_score_stat(simmed_play, move_get_score(best_play),
                               move_get_tiles_played(best_play) == RACK_SIZE,
                               ply);
  }

  const Equity spread =
      player_get_score(game_get_player(game, simmer->initial_player)) -
      player_get_score(game_get_player(game, 1 - simmer->initial_player));
  simmed_play_add_equity_stat(simmed_play, simmer->initial_spread, spread,
                              leftover);
  const double wpct = simmed_play_add_win_pct_stat(
      simmer->win_pcts, simmed_play, spread, leftover,
      game_get_game_end_reason(game),
      // number of tiles unseen to us: bag tiles + tiles on opp rack.
      bag_get_tiles(game_get_bag(game)) +
          rack_get_total_letters(player_get_rack(
              game_get_player(game, 1 - simmer->initial_player))),
      plies % 2);
  // reset to first state. we only need to restore one backup.
  game_unplay_last_move(game);
  return_rack_to_bag(game, player_off_turn_index);

  const int print_interval =
      thread_control_get_print_info_interval(simmer->thread_control);
  if (print_interval > 0 && sample_count % print_interval == 0) {
    print_ucgi_sim_stats(
        simmer_worker->game, simmer->sim_results, simmer->thread_control,
        (double)sim_results_get_node_count(simmer->sim_results) /
            thread_control_get_seconds_elapsed(simmer->thread_control),
        false);
  }

  return wpct;
}

bool rv_sim_mark_as_epigon_if_similar(RandomVariables *rvs, const int leader,
                                      const int i) {
  Simmer *simmer = (Simmer *)rvs->data;
  const bool plays_are_similar = simmer_plays_are_similar(simmer, leader, i);
  if (plays_are_similar) {
    simmed_play_set_is_epigon(
        sim_results_get_simmed_play(simmer->sim_results, i));
  }
  return plays_are_similar;
}

bool rv_sim_is_epigon(const RandomVariables *rvs, const int i) {
  const Simmer *simmer = (Simmer *)rvs->data;
  return simmed_play_get_is_epigon(
      sim_results_get_simmed_play(simmer->sim_results, i));
}

void rv_sim_destroy(RandomVariables *rvs) {
  Simmer *simmer = (Simmer *)rvs->data;
  rack_destroy(simmer->known_opp_rack);
  const int num_threads = thread_control_get_threads(simmer->thread_control);
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    simmer_worker_destroy(simmer->workers[thread_index]);
  }
  free(simmer->workers);
  free(simmer);
}

RandomVariables *rv_sim_create(RandomVariables *rvs, const SimArgs *sim_args,
                               SimResults *sim_results) {
  rvs->sample_func = rv_sim_sample;
  rvs->similar_func = rv_sim_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_sim_is_epigon;
  rvs->destroy_data_func = rv_sim_destroy;

  rvs->num_rvs = move_list_get_count(sim_args->move_list);

  Simmer *simmer = malloc_or_die(sizeof(Simmer));
  ThreadControl *thread_control = sim_args->thread_control;

  simmer->initial_player = game_get_player_on_turn_index(sim_args->game);
  Player *player = game_get_player(sim_args->game, simmer->initial_player);
  Player *opponent =
      game_get_player(sim_args->game, 1 - simmer->initial_player);

  simmer->initial_spread =
      player_get_score(player) - player_get_score(opponent);

  Rack *known_opp_rack = sim_args->known_opp_rack;
  if (known_opp_rack && !rack_is_empty(known_opp_rack)) {
    simmer->known_opp_rack = rack_duplicate(known_opp_rack);
  } else {
    simmer->known_opp_rack = NULL;
  }

  const int num_threads = thread_control_get_threads(thread_control);
  simmer->workers = malloc_or_die((sizeof(SimmerWorker *)) * (num_threads));
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    simmer->workers[thread_index] = simmer_create_worker(sim_args->game);
  }

  simmer->win_pcts = sim_args->win_pcts;

  simmer->thread_control = thread_control;

  sim_results_reset(sim_args->move_list, sim_results, sim_args->num_plies,
                    thread_control_get_seed(thread_control));
  simmer->sim_results = sim_results;

  rvs->data = simmer;
  return rvs;
}

RandomVariables *rvs_create(const RandomVariablesArgs *rvs_args) {
  RandomVariables *rvs = malloc_or_die(sizeof(RandomVariables));
  // The num_rvs field will be overwritten in the rv_sim_create function
  // since it is cumbersome and unnecessary for the caller to set
  // rvs_args->num_rvs for simmed plays.
  rvs->num_rvs = rvs_args->num_rvs;
  atomic_store(&rvs->total_samples, 0);
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_UNIFORM:
    rv_uniform_create(rvs, rvs_args->seed);
    break;
  case RANDOM_VARIABLES_UNIFORM_PREDETERMINED:
    rv_uniform_predetermined_create(rvs, rvs_args->samples,
                                    rvs_args->num_samples);
    break;
  case RANDOM_VARIABLES_NORMAL:
    rv_normal_create(rvs, rvs_args->seed, rvs_args->means_and_vars);
    break;
  case RANDOM_VARIABLES_NORMAL_PREDETERMINED:
    rv_normal_predetermined_create(rvs, rvs_args->samples,
                                   rvs_args->num_samples,
                                   rvs_args->means_and_vars);
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    rv_sim_create(rvs, rvs_args->sim_args, rvs_args->sim_results);
    break;
  }
  return rvs;
}

void rvs_destroy(RandomVariables *rvs) {
  rvs->destroy_data_func(rvs);
  free(rvs);
}

double rvs_sample(RandomVariables *rvs, const uint64_t k,
                  const int thread_index, BAILogger *bai_logger) {
  uint64_t prev_total_samples = atomic_fetch_add(&rvs->total_samples, 1);
  return rvs->sample_func(rvs, k, thread_index, prev_total_samples + 1,
                          bai_logger);
}

void rvs_reset(RandomVariables *rvs) { rvs->total_samples = 0; }

// Returns the similarity between the leader and the i-th arm and marks
// the arm accordingly.
bool rvs_mark_as_epigon_if_similar(RandomVariables *rvs, const int leader,
                                   const int i) {
  return rvs->similar_func(rvs, leader, i);
}

bool rvs_is_epigon(const RandomVariables *rvs, const int i) {
  return rvs->is_epigon_func(rvs, i);
}

int rvs_get_num_rvs(const RandomVariables *rvs) { return rvs->num_rvs; }

// NOT THREAD SAFE: caller is responsible for ensuring thread safety.
uint64_t rvs_get_total_samples(const RandomVariables *rvs) {
  return rvs->total_samples;
}