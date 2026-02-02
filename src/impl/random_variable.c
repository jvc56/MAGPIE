#include "random_variable.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/game_defs.h"
#include "../ent/alias_method.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../ent/xoshiro.h"
#include "../str/sim_string.h"
#include "../util/io_util.h"
#include "bai_logger.h"
#include "gameplay.h"
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SIMILARITY_EPSILON 1e-6

typedef double (*rvs_sample_func_t)(RandomVariables *, const uint64_t,
                                    const int, const uint64_t, BAILogger *);
typedef bool (*rvs_similar_func_t)(RandomVariables *, const int, const int);
typedef void (*rvs_destroy_data_func_t)(RandomVariables *);

struct RandomVariables {
  uint64_t num_rvs;
  atomic_uint_fast64_t total_samples;
  rvs_sample_func_t sample_func;
  rvs_similar_func_t similar_func;
  rvs_destroy_data_func_t destroy_data_func;
  void *data;
};

double uniform_sample(XoshiroPRNG *prng, cpthread_mutex_t *mutex) {
  cpthread_mutex_lock(mutex);
  double result = (double)prng_next(prng) / ((double)UINT64_MAX);
  cpthread_mutex_unlock(mutex);
  return result;
}

typedef struct RVUniform {
  XoshiroPRNG *xoshiro_prng;
  cpthread_mutex_t mutex;
} RVUniform;

double rv_uniform_sample(RandomVariables *rvs,
                         const uint64_t __attribute__((unused)) k,
                         const int __attribute__((unused)) thread_index,
                         const uint64_t __attribute__((unused)) sample_count,
                         BAILogger __attribute__((unused)) * bai_logger) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  return uniform_sample(rv_uniform->xoshiro_prng, &rv_uniform->mutex);
}

bool rv_uniform_are_similar(RandomVariables __attribute__((unused)) * rvs,
                            const int __attribute__((unused)) i,
                            const int __attribute__((unused)) j) {
  return false;
}

void rv_uniform_destroy(RandomVariables *rvs) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  prng_destroy(rv_uniform->xoshiro_prng);
  free(rv_uniform);
}

void rv_uniform_create(RandomVariables *rvs, const uint64_t seed) {
  rvs->sample_func = rv_uniform_sample;
  rvs->similar_func = rv_uniform_are_similar;
  rvs->destroy_data_func = rv_uniform_destroy;
  RVUniform *rv_uniform = malloc_or_die(sizeof(RVUniform));
  rv_uniform->xoshiro_prng = prng_create(seed);
  cpthread_mutex_init(&rv_uniform->mutex);
  rvs->data = rv_uniform;
}

void rv_uniform_reset(RandomVariables *rvs, const uint64_t seed) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  prng_seed(rv_uniform->xoshiro_prng, seed);
}

typedef struct RVUniformPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
  cpthread_mutex_t mutex;
} RVUniformPredetermined;

double rv_uniform_predetermined_sample(
    RandomVariables *rvs, const uint64_t __attribute__((unused)) k,
    const int __attribute__((unused)) thread_index,
    const uint64_t __attribute__((unused)) sample_count,
    BAILogger __attribute__((unused)) * bai_logger) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  cpthread_mutex_lock(&rv_uniform_predetermined->mutex);
  if (rv_uniform_predetermined->index >=
      rv_uniform_predetermined->num_samples) {
    log_fatal("ran out of uniform predetermined samples");
  }
  const uint64_t index = rv_uniform_predetermined->index++;
  cpthread_mutex_unlock(&rv_uniform_predetermined->mutex);
  return rv_uniform_predetermined->samples[index];
}

bool rv_uniform_predetermined_are_similar(RandomVariables
                                              __attribute__((unused)) *
                                              rvs,
                                          const int __attribute__((unused)) i,
                                          const int __attribute__((unused)) j) {
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
  rvs->similar_func = rv_uniform_predetermined_are_similar;
  rvs->destroy_data_func = rv_uniform_predetermined_destroy;
  RVUniformPredetermined *rv_uniform_predetermined =
      malloc_or_die(sizeof(RVUniformPredetermined));
  rv_uniform_predetermined->num_samples = num_samples;
  rv_uniform_predetermined->index = 0;
  rv_uniform_predetermined->samples =
      malloc_or_die(rv_uniform_predetermined->num_samples * sizeof(double));
  memcpy(rv_uniform_predetermined->samples, samples,
         rv_uniform_predetermined->num_samples * sizeof(double));
  cpthread_mutex_init(&rv_uniform_predetermined->mutex);
  rvs->data = rv_uniform_predetermined;
}

void rv_uniform_predetermined_reset(RandomVariables *rvs) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  rv_uniform_predetermined->index = 0;
}

typedef struct RVNormal {
  XoshiroPRNG *xoshiro_prng;
  double *means_and_vars;
  cpthread_mutex_t mutex;
} RVNormal;

double rv_normal_sample(RandomVariables *rvs, const uint64_t k,
                        const int __attribute__((unused)) thread_index,
                        const uint64_t __attribute__((unused)) sample_count,
                        BAILogger __attribute__((unused)) * bai_logger) {
  // Implements the Box-Muller transform
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  double u = 0.0;
  double s = 2.0;
  while (s >= 1.0 || s == 0.0) {
    u = 2.0 * uniform_sample(rv_normal->xoshiro_prng, &rv_normal->mutex) - 1.0;
    const double v =
        2.0 * uniform_sample(rv_normal->xoshiro_prng, &rv_normal->mutex) - 1.0;
    s = u * u + v * v;
  }
  s = sqrt(-2.0 * log(s) / s);
  return rv_normal->means_and_vars[k * 2] +
         rv_normal->means_and_vars[k * 2 + 1] * u * s;
}

bool rv_normal_are_similar(RandomVariables *rvs, const int i, const int j) {
  if (i == j) {
    return false;
  }
  const RVNormal *rv_normal = (RVNormal *)rvs->data;
  return fabs(rv_normal->means_and_vars[(ptrdiff_t)(i * 2)] -
              rv_normal->means_and_vars[(ptrdiff_t)(j * 2)]) <
             SIMILARITY_EPSILON &&
         fabs(rv_normal->means_and_vars[(ptrdiff_t)(i * 2 + 1)] -
              rv_normal->means_and_vars[(ptrdiff_t)(j * 2 + 1)]) <
             SIMILARITY_EPSILON;
}

void rv_normal_destroy(RandomVariables *rvs) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  prng_destroy(rv_normal->xoshiro_prng);
  free(rv_normal->means_and_vars);
  free(rv_normal);
}

void rv_normal_create(RandomVariables *rvs, const uint64_t seed,
                      const double *means_and_vars) {
  rvs->sample_func = rv_normal_sample;
  rvs->similar_func = rv_normal_are_similar;
  rvs->destroy_data_func = rv_normal_destroy;
  RVNormal *rv_normal = malloc_or_die(sizeof(RVNormal));
  rv_normal->xoshiro_prng = prng_create(seed);
  rv_normal->means_and_vars = malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memcpy(rv_normal->means_and_vars, means_and_vars,
         rvs->num_rvs * 2 * sizeof(double));
  cpthread_mutex_init(&rv_normal->mutex);
  rvs->data = rv_normal;
}

void rv_normal_reset(RandomVariables *rvs, const uint64_t seed) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  prng_seed(rv_normal->xoshiro_prng, seed);
}

typedef struct RVNormalPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
  double *means_and_vars;
  cpthread_mutex_t mutex;
} RVNormalPredetermined;

double rv_normal_predetermined_sample(RandomVariables *rvs, const uint64_t k,
                                      const int
                                      __attribute__((unused)) thread_index,
                                      const uint64_t
                                      __attribute__((unused)) sample_count,
                                      BAILogger *bai_logger) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  cpthread_mutex_lock(&rv_normal_predetermined->mutex);
  if (rv_normal_predetermined->index >= rv_normal_predetermined->num_samples) {
    log_fatal("ran out of normal predetermined samples");
  }
  const uint64_t index = rv_normal_predetermined->index++;
  cpthread_mutex_unlock(&rv_normal_predetermined->mutex);
  const double mean = rv_normal_predetermined->means_and_vars[k * 2];
  const double sigma2 = rv_normal_predetermined->means_and_vars[k * 2 + 1];
  const double sample = rv_normal_predetermined->samples[index];
  const double result = mean + sqrt(sigma2) * sample;
  bai_logger_log_title(bai_logger, "DETERMINISTIC_SAMPLE");
  bai_logger_log_int(bai_logger, "index", (int)(index + 1));
  bai_logger_log_int(bai_logger, "arm", (int)k + 1);
  bai_logger_log_double(bai_logger, "s", result);
  bai_logger_log_double(bai_logger, "u", mean);
  bai_logger_log_double(bai_logger, "sigma2", sigma2);
  bai_logger_log_double(bai_logger, "samp", sample);
  bai_logger_flush(bai_logger);
  return result;
}

bool rv_normal_predetermined_are_similar(RandomVariables *rvs, const int i,
                                         const int j) {
  if (i == j) {
    return false;
  }
  const RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  return fabs(rv_normal_predetermined->means_and_vars[(ptrdiff_t)(i * 2)] -
              rv_normal_predetermined->means_and_vars[(ptrdiff_t)(j * 2)]) <
             SIMILARITY_EPSILON &&
         fabs(rv_normal_predetermined->means_and_vars[(ptrdiff_t)(i * 2 + 1)] -
              rv_normal_predetermined->means_and_vars[(ptrdiff_t)(j * 2 + 1)]) <
             SIMILARITY_EPSILON;
}

void rv_normal_predetermined_destroy(RandomVariables *rvs) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  free(rv_normal_predetermined->samples);
  free(rv_normal_predetermined->means_and_vars);
  free(rv_normal_predetermined);
}

void rv_normal_predetermined_create(RandomVariables *rvs, const double *samples,
                                    const uint64_t num_samples,
                                    const double *means_and_vars) {
  rvs->sample_func = rv_normal_predetermined_sample;
  rvs->similar_func = rv_normal_predetermined_are_similar;
  rvs->destroy_data_func = rv_normal_predetermined_destroy;
  RVNormalPredetermined *rv_normal_predetermined =
      malloc_or_die(sizeof(RVNormalPredetermined));
  rv_normal_predetermined->num_samples = num_samples;
  rv_normal_predetermined->index = 0;
  rv_normal_predetermined->samples =
      malloc_or_die(rv_normal_predetermined->num_samples * sizeof(double));
  memcpy(rv_normal_predetermined->samples, samples,
         rv_normal_predetermined->num_samples * sizeof(double));
  rv_normal_predetermined->means_and_vars =
      malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memcpy(rv_normal_predetermined->means_and_vars, means_and_vars,
         rvs->num_rvs * 2 * sizeof(double));
  cpthread_mutex_init(&rv_normal_predetermined->mutex);
  rvs->data = rv_normal_predetermined;
}

void rv_normal_predetermined_reset(RandomVariables *rvs) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  rv_normal_predetermined->index = 0;
}

typedef struct SimmerWorker {
  Game *game;
  MoveList *move_list;
  XoshiroPRNG *prng;
} SimmerWorker;

typedef struct Simmer {
  Equity initial_spread;
  int initial_player;
  int dist_size;
  Rack *known_opp_rack;
  SimmerWorker **workers;
  // Owned by the caller
  const WinPct *win_pcts;
  bool use_inference;
  bool use_alias_method;
  const InferenceResults *inference_results;
  int num_threads;
  int print_interval;
  int max_num_display_plays;
  ThreadControl *thread_control;
  SimResults *sim_results;
  dual_lexicon_mode_t dual_lexicon_mode;
  // KWG of the initial player (used in IGNORANT mode to override opponent moves)
  const KWG *initial_player_kwg;
} Simmer;

SimmerWorker *simmer_create_worker(const Game *game) {
  SimmerWorker *simmer_worker = malloc_or_die(sizeof(SimmerWorker));
  simmer_worker->game = game_duplicate(game);
  game_set_backup_mode(simmer_worker->game, BACKUP_MODE_SIMULATION);
  simmer_worker->move_list = move_list_create(1);
  simmer_worker->prng = prng_create(0);
  return simmer_worker;
}

void simmer_reset_worker(SimmerWorker *simmer_worker, const Game *game) {
  game_copy(simmer_worker->game, game);
}

void simmer_worker_destroy(SimmerWorker *simmer_worker) {
  if (!simmer_worker) {
    return;
  }
  game_destroy(simmer_worker->game);
  move_list_destroy(simmer_worker->move_list);
  prng_destroy(simmer_worker->prng);
  free(simmer_worker);
}

double rv_sim_sample(RandomVariables *rvs, const uint64_t play_index,
                     const int thread_index, const uint64_t sample_count,
                     BAILogger __attribute__((unused)) * bai_logger) {
  Simmer *simmer = (Simmer *)rvs->data;
  SimResults *sim_results = simmer->sim_results;
  SimmedPlay *simmed_play =
      sim_results_get_simmed_play(sim_results, (int)play_index);
  SimmerWorker *simmer_worker = simmer->workers[thread_index];
  Game *game = simmer_worker->game;
  MoveList *move_list = simmer_worker->move_list;
  const int plies = sim_results_get_num_plies(sim_results);

  // This will shuffle the bag, so there is no need
  // to call bag_shuffle explicitly.
  const uint64_t seed = simmed_play_get_seed(simmed_play);
  prng_seed(simmer_worker->prng, seed);
  game_seed(game, seed);

  int player_off_turn_index = 1 - game_get_player_on_turn_index(game);
  if (simmer->use_alias_method) {
    Rack inferred_rack;
    rack_set_dist_size(&inferred_rack, simmer->dist_size);
    if (alias_method_sample(
            inference_results_get_alias_method(simmer->inference_results),
            simmer_worker->prng, &inferred_rack)) {
      set_random_rack(game, player_off_turn_index, &inferred_rack);
    }
  } else {
    set_random_rack(game, player_off_turn_index, simmer->known_opp_rack);
  }

  Equity leftover = 0;
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  // For one-ply sims, we need to account for the candidate move's leave value
  if (plies == 1) {
    Rack candidate_rack;
    const Player *player_on_turn =
        game_get_player(game, simmer->initial_player);
    rack_copy(&candidate_rack, player_get_rack(player_on_turn));
    leftover += get_leave_value_for_move(player_get_klv(player_on_turn),
                                         simmed_play_get_move(simmed_play),
                                         &candidate_rack);
  }
  // play move
  play_move(simmed_play_get_move(simmed_play), game, NULL);
  sim_results_increment_node_count(sim_results);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  // further plies will NOT be backed up.
  Rack spare_rack;
  for (int ply = 0; ply < plies; ply++) {
    const int player_on_turn_index = game_get_player_on_turn_index(game);
    const Player *player_on_turn = game_get_player(game, player_on_turn_index);

    if (game_over(game)) {
      break;
    }

    // In IGNORANT mode, when opponent is on turn, use the initial player's KWG
    // so opponent "plays dumb" - doesn't exploit lexicon-specific words.
    // In INFORMED mode, opponent uses their actual KWG.
    const Move *best_play;
    if (simmer->dual_lexicon_mode == DUAL_LEXICON_MODE_IGNORANT &&
        player_on_turn_index != simmer->initial_player) {
      best_play = get_top_equity_move_with_kwg_override(
          game, thread_index, move_list, simmer->initial_player_kwg);
    } else {
      best_play = get_top_equity_move(game, thread_index, move_list);
    }
    rack_copy(&spare_rack, player_get_rack(player_on_turn));

    play_move(best_play, game, NULL);
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
    simmed_play_add_stats_for_ply(simmed_play, ply, best_play);
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
      bag_get_letters(game_get_bag(game)) +
          rack_get_total_letters(player_get_rack(
              game_get_player(game, 1 - simmer->initial_player))),
      plies % 2);
  // reset to first state. we only need to restore one backup.
  game_unplay_last_move(game);
  return_rack_to_bag(game, player_off_turn_index);

  if (simmer->print_interval > 0 &&
      sample_count % simmer->print_interval == 0) {
    sim_results_print(simmer->thread_control, simmer_worker->game,
                      simmer->sim_results, simmer->max_num_display_plays, true);
  }
  sim_results_increment_iteration_count(sim_results);

  return wpct;
}

bool rv_sim_are_similar(RandomVariables *rvs, const int i, const int j) {
  const Simmer *simmer = (Simmer *)rvs->data;
  return sim_results_plays_are_similar(simmer->sim_results, i, j);
}

void rv_sim_destroy(RandomVariables *rvs) {
  Simmer *simmer = (Simmer *)rvs->data;
  rack_destroy(simmer->known_opp_rack);
  for (int thread_index = 0; thread_index < simmer->num_threads;
       thread_index++) {
    simmer_worker_destroy(simmer->workers[thread_index]);
  }
  free(simmer->workers);
  free(simmer);
}

RandomVariables *rv_sim_create(RandomVariables *rvs, const SimArgs *sim_args,
                               SimResults *sim_results) {
  rvs->sample_func = rv_sim_sample;
  rvs->similar_func = rv_sim_are_similar;
  rvs->destroy_data_func = rv_sim_destroy;

  rvs->num_rvs = move_list_get_count(sim_args->move_list);

  Simmer *simmer = malloc_or_die(sizeof(Simmer));
  ThreadControl *thread_control = sim_args->thread_control;

  simmer->initial_player = game_get_player_on_turn_index(sim_args->game);
  const Player *player =
      game_get_player(sim_args->game, simmer->initial_player);
  const Player *opponent =
      game_get_player(sim_args->game, 1 - simmer->initial_player);

  simmer->initial_spread =
      player_get_score(player) - player_get_score(opponent);

  const Rack *known_opp_rack = sim_args->known_opp_rack;
  if (known_opp_rack && !rack_is_empty(known_opp_rack)) {
    simmer->known_opp_rack = rack_duplicate(known_opp_rack);
  } else {
    simmer->known_opp_rack = NULL;
  }

  simmer->dist_size = ld_get_size(game_get_ld(sim_args->game));

  simmer->num_threads = sim_args->num_threads;
  simmer->print_interval = sim_args->print_interval;
  simmer->max_num_display_plays = sim_args->max_num_display_plays;

  simmer->workers =
      malloc_or_die((sizeof(SimmerWorker *)) * (simmer->num_threads));
  for (int thread_index = 0; thread_index < simmer->num_threads;
       thread_index++) {
    simmer->workers[thread_index] = simmer_create_worker(sim_args->game);
  }

  simmer->win_pcts = sim_args->win_pcts;
  simmer->use_inference = sim_args->use_inference;
  simmer->use_alias_method =
      simmer->use_inference &&
      (!simmer->known_opp_rack || rack_is_empty(simmer->known_opp_rack));
  simmer->inference_results = sim_args->inference_results;

  simmer->thread_control = thread_control;

  // Store dual-lexicon mode and initial player's KWG for IGNORANT mode
  simmer->dual_lexicon_mode = sim_args->dual_lexicon_mode;
  simmer->initial_player_kwg = player_get_kwg(player);

  sim_results_reset(sim_args->move_list, sim_results, sim_args->num_plies,
                    sim_args->seed, sim_args->use_heat_map);
  simmer->sim_results = sim_results;

  rvs->data = simmer;
  return rvs;
}

void rv_sim_reset(RandomVariables *rvs, const SimArgs *sim_args) {
  Simmer *simmer = (Simmer *)rvs->data;
  rvs->num_rvs = move_list_get_count(sim_args->move_list);

  simmer->initial_player = game_get_player_on_turn_index(sim_args->game);
  const Player *player =
      game_get_player(sim_args->game, simmer->initial_player);
  const Player *opponent =
      game_get_player(sim_args->game, 1 - simmer->initial_player);

  simmer->initial_spread =
      player_get_score(player) - player_get_score(opponent);

  const Rack *known_opp_rack = sim_args->known_opp_rack;
  if (known_opp_rack && !rack_is_empty(known_opp_rack)) {
    if (!simmer->known_opp_rack) {
      simmer->known_opp_rack = rack_duplicate(known_opp_rack);
    } else {
      rack_copy(simmer->known_opp_rack, known_opp_rack);
    }
  } else {
    if (simmer->known_opp_rack) {
      // FIXME: avoid repeated alloc/free if resetting multiple times
      rack_destroy(simmer->known_opp_rack);
    }
    simmer->known_opp_rack = NULL;
  }

  for (int thread_index = 0; thread_index < simmer->num_threads;
       thread_index++) {
    simmer_reset_worker(simmer->workers[thread_index], sim_args->game);
  }

  simmer->use_inference = sim_args->use_inference;
  simmer->use_alias_method =
      simmer->use_inference &&
      (!simmer->known_opp_rack || rack_is_empty(simmer->known_opp_rack));

  // Update dual-lexicon mode and initial player's KWG
  simmer->dual_lexicon_mode = sim_args->dual_lexicon_mode;
  simmer->initial_player_kwg = player_get_kwg(player);

  sim_results_reset(sim_args->move_list, simmer->sim_results,
                    sim_args->num_plies, sim_args->seed,
                    sim_args->use_heat_map);
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

void rvs_reset(RandomVariables *rvs, const RandomVariablesArgs *rvs_args) {
  rvs->num_rvs = rvs_args->num_rvs;
  atomic_store(&rvs->total_samples, 0);
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_UNIFORM:
    rv_uniform_reset(rvs, rvs_args->seed);
    break;
  case RANDOM_VARIABLES_UNIFORM_PREDETERMINED:
    rv_uniform_predetermined_reset(rvs);
    break;
  case RANDOM_VARIABLES_NORMAL:
    rv_normal_reset(rvs, rvs_args->seed);
    break;
  case RANDOM_VARIABLES_NORMAL_PREDETERMINED:
    rv_normal_predetermined_reset(rvs);
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    rv_sim_reset(rvs, rvs_args->sim_args);
    break;
  }
}

void rvs_destroy(RandomVariables *rvs) {
  if (!rvs) {
    return;
  }
  if (rvs->destroy_data_func == NULL) {
    return;
  }
  rvs->destroy_data_func(rvs);
  free(rvs);
}

double rvs_sample(RandomVariables *rvs, const uint64_t k,
                  const int thread_index, BAILogger *bai_logger) {
  uint64_t prev_total_samples = atomic_fetch_add(&rvs->total_samples, 1);
  return rvs->sample_func(rvs, k, thread_index, prev_total_samples + 1,
                          bai_logger);
}

bool rvs_are_similar(RandomVariables *rvs, const int i, const int j) {
  return rvs->similar_func(rvs, i, j);
}

uint64_t rvs_get_num_rvs(const RandomVariables *rvs) { return rvs->num_rvs; }

// NOT THREAD SAFE: caller is responsible for ensuring thread safety.
uint64_t rvs_get_total_samples(const RandomVariables *rvs) {
  return rvs->total_samples;
}