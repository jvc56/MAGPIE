#include "simmer.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/simmer_defs.h"
#include "../def/thread_control_defs.h"

#include "../def/rack_defs.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/timer.h"
#include "../ent/validated_move.h"
#include "../ent/win_pct.h"

#include "config.h"
#include "gameplay.h"
#include "move_gen.h"

#include "../str/game_string.h"
#include "../str/sim_string.h"

#include "../util/log.h"
#include "../util/math_util.h"
#include "../util/util.h"

#define MAX_STOPPING_ITERATION_CT 4000
#define PER_PLY_STOPPING_SCALING 1250
#define SIMILAR_PLAYS_ITER_CUTOFF 1000

typedef struct Simmer {
  int initial_spread;
  int initial_player;
  int max_iterations;
  double zval;
  int threads;
  uint64_t seed;
  pthread_mutex_t iteration_count_mutex;

  Rack *known_opp_rack;
  Rack *similar_plays_rack;
  int *play_similarity_cache;

  // Owned by the caller
  const WinPct *win_pcts;
  ThreadControl *thread_control;
  SimResults *sim_results;
} Simmer;

typedef struct SimmerWorker {
  int thread_index;
  Game *game;
  MoveList *move_list;
  Rack *rack_placeholder;
  Simmer *simmer;
} SimmerWorker;

Simmer *create_simmer(const SimArgs *args, Game *game, int num_simmed_plays,
                      SimResults *sim_results) {
  Simmer *simmer = malloc_or_die(sizeof(Simmer));
  ThreadControl *thread_control = args->thread_control;
  int ld_size = ld_get_size(game_get_ld(game));

  simmer->initial_player = game_get_player_on_turn_index(game);
  Player *player = game_get_player(game, simmer->initial_player);
  Player *opponent = game_get_player(game, 1 - simmer->initial_player);

  simmer->initial_spread =
      player_get_score(player) - player_get_score(opponent);
  simmer->max_iterations = args->max_iterations;
  simmer->zval = p_to_z(args->stop_cond_pct);
  simmer->threads = thread_control_get_threads(thread_control);
  simmer->seed = args->seed;
  pthread_mutex_init(&simmer->iteration_count_mutex, NULL);

  Rack *known_opp_rack = args->known_opp_rack;

  if (known_opp_rack && !rack_is_empty(known_opp_rack)) {
    simmer->known_opp_rack = rack_duplicate(known_opp_rack);
  } else {
    simmer->known_opp_rack = NULL;
  }

  simmer->similar_plays_rack = rack_create(ld_size);

  simmer->play_similarity_cache =
      malloc_or_die(sizeof(int) * num_simmed_plays * num_simmed_plays);
  for (int i = 0; i < num_simmed_plays; i++) {
    for (int j = 0; j < num_simmed_plays; j++) {
      if (i == j) {
        simmer->play_similarity_cache[i * num_simmed_plays + j] =
            PLAYS_IDENTICAL;
      } else {
        simmer->play_similarity_cache[i * num_simmed_plays + j] =
            UNINITIALIZED_SIMILARITY;
      }
    }
  }

  simmer->win_pcts = args->win_pcts;

  simmer->thread_control = thread_control;

  sim_results_reset(args->move_list, sim_results, num_simmed_plays,
                    args->num_plies, simmer->zval);

  simmer->sim_results = sim_results;

  return simmer;
}

void destroy_simmer(Simmer *simmer) {
  if (!simmer) {
    return;
  }
  rack_destroy(simmer->similar_plays_rack);
  rack_destroy(simmer->known_opp_rack);
  free(simmer->play_similarity_cache);
  free(simmer);
}

SimmerWorker *create_simmer_worker(const Game *game, Simmer *simmer,
                                   int worker_index) {
  SimmerWorker *simmer_worker = malloc_or_die(sizeof(SimmerWorker));

  // Thread index
  simmer_worker->thread_index = worker_index;

  // Game
  simmer_worker->game = game_duplicate(game);
  game_set_backup_mode(simmer_worker->game, BACKUP_MODE_SIMULATION);
  bag_seed_for_worker(game_get_bag(simmer_worker->game), simmer->seed,
                      worker_index);

  // MoveList
  simmer_worker->move_list = move_list_create(1);

  int ld_size = ld_get_size(game_get_ld(simmer_worker->game));

  // Rack placeholder
  simmer_worker->rack_placeholder = rack_create(ld_size);

  // Simmer
  simmer_worker->simmer = simmer;

  return simmer_worker;
}

void destroy_simmer_worker(SimmerWorker *simmer_worker) {
  if (!simmer_worker) {
    return;
  }
  game_destroy(simmer_worker->game);
  move_list_destroy(simmer_worker->move_list);
  rack_destroy(simmer_worker->rack_placeholder);
  free(simmer_worker);
}

bool plays_are_similar(const SimmedPlay *m1, const SimmedPlay *m2,
                       Simmer *simmer) {
  // look in the cache first
  int m1_play_id = simmed_play_get_id(m1);
  int m2_play_id = simmed_play_get_id(m2);

  Move *m1_move = simmed_play_get_move(m1);
  Move *m2_move = simmed_play_get_move(m2);

  SimResults *sim_results = simmer->sim_results;

  int number_of_plays = sim_results_get_number_of_plays(sim_results);

  int cache_value =
      simmer->play_similarity_cache[m1_play_id + number_of_plays * m2_play_id];
  assert(cache_value != PLAYS_IDENTICAL);
  if (cache_value == PLAYS_SIMILAR) {
    return true;
  } else if (cache_value == PLAYS_NOT_SIMILAR) {
    return false;
  }
  int cache_index1 = m1_play_id + number_of_plays * m2_play_id;
  int cache_index2 = m2_play_id + number_of_plays * m1_play_id;

  if (move_get_type(m1_move) != move_get_type(m2_move)) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }

  // Otherwise, we must compute play similarity and fill in the cache.
  // two plays are "similar" if they use the same tiles, and they start at
  // the same square.
  if (!(move_get_dir(m1_move) == move_get_dir(m2_move) &&
        move_get_col_start(m1_move) == move_get_col_start(m2_move) &&
        move_get_row_start(m1_move) == move_get_row_start(m2_move))) {

    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }
  if (!(move_get_tiles_played(m1_move) == move_get_tiles_played(m2_move) &&
        move_get_tiles_length(m1_move) == move_get_tiles_length(m2_move))) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }

  // Create a rack from m1, then subtract the rack from m2. The final
  // rack should have all zeroes.
  rack_reset(simmer->similar_plays_rack);
  for (int i = 0; i < move_get_tiles_length(m1_move); i++) {
    uint8_t tile = move_get_tile(m1_move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (get_is_blanked(ml)) {
      ml = 0;
    }
    rack_add_letter(simmer->similar_plays_rack, ml);
  }

  for (int i = 0; i < move_get_tiles_length(m2_move); i++) {
    uint8_t tile = move_get_tile(m1_move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (get_is_blanked(ml)) {
      ml = 0;
    }
    rack_take_letter(simmer->similar_plays_rack, ml);
  }

  for (int i = 0; i < rack_get_dist_size(simmer->similar_plays_rack); i++) {
    if (rack_get_letter(simmer->similar_plays_rack, i) != 0) {
      simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
      simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
      return false;
    }
  }
  simmer->play_similarity_cache[cache_index1] = PLAYS_SIMILAR;
  simmer->play_similarity_cache[cache_index2] = PLAYS_SIMILAR;

  return true;
}

bool is_multithreaded(const Simmer *simmer) { return simmer->threads > 1; }

bool handle_potential_stopping_condition(Simmer *simmer) {
  bool use_stopping_cond = simmer->zval < 100;
  SimResults *sim_results = simmer->sim_results;
  int number_of_plays = sim_results_get_number_of_plays(sim_results);
  int total_ignored = 0;

  sim_results_lock_simmed_plays(simmer->sim_results);
  bool success = sim_results_sort_plays_by_win_rate(simmer->sim_results);
  if (!success) {
    log_fatal("cannot sort null simmed plays");
  }

  const SimmedPlay *tentative_winner =
      sim_results_get_simmed_play(sim_results, 0);
  double mu = stat_get_mean(simmed_play_get_win_pct_stat(tentative_winner));
  double std_error = stat_get_stderr(
      simmed_play_get_win_pct_stat(tentative_winner), simmer->zval);

  for (int i = 1; i < number_of_plays; i++) {
    SimmedPlay *simmed_play = sim_results_get_simmed_play(sim_results, i);
    if (simmed_play_get_ignore(simmed_play)) {
      total_ignored++;
      continue;
    }
    double mu_i = stat_get_mean(simmed_play_get_win_pct_stat(simmed_play));
    double std_error_i = stat_get_stderr(
        simmed_play_get_win_pct_stat(simmed_play), simmer->zval);

    if ((use_stopping_cond && (mu - std_error) > (mu_i + std_error_i)) ||
        (sim_results_get_iteration_count(sim_results) >
             SIMILAR_PLAYS_ITER_CUTOFF &&
         plays_are_similar(tentative_winner, simmed_play, simmer))) {
      simmed_play_set_ignore(simmed_play, is_multithreaded(simmer));
      total_ignored++;
    }
  }
  sim_results_unlock_simmed_plays(simmer->sim_results);
  log_debug("total ignored: %d\n", total_ignored);

  return total_ignored >= number_of_plays - 1;
}

void sim_single_iteration(SimmerWorker *simmer_worker) {
  Game *game = simmer_worker->game;
  Rack *rack_placeholder = simmer_worker->rack_placeholder;
  Simmer *simmer = simmer_worker->simmer;
  SimResults *sim_results = simmer->sim_results;
  int plies = sim_results_get_max_plies(sim_results);
  int number_of_plays = sim_results_get_number_of_plays(sim_results);

  // set random rack for opponent (throw in rack, bag_shuffle, draw new tiles).
  set_random_rack(game, 1 - game_get_player_on_turn_index(game),
                  simmer->known_opp_rack);
  // need a new bag_shuffle for every iteration:
  Bag *bag = game_get_bag(game);
  bag_shuffle(bag);

  for (int i = 0; i < number_of_plays; i++) {
    SimmedPlay *simmed_play = sim_results_get_simmed_play(sim_results, i);
    if (simmed_play_get_ignore(simmed_play)) {
      continue;
    }

    double leftover = 0.0;
    game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
    // play move
    play_move(simmed_play_get_move(simmed_play), game, NULL);
    sim_results_increment_node_count(sim_results);
    game_set_backup_mode(game, BACKUP_MODE_OFF);
    // further plies will NOT be backed up.
    for (int ply = 0; ply < plies; ply++) {
      int player_on_turn_index = game_get_player_on_turn_index(game);
      Player *player_on_turn = game_get_player(game, player_on_turn_index);

      if (game_over(game)) {
        break;
      }

      const Move *best_play = get_top_equity_move(
          game, simmer_worker->thread_index, simmer_worker->move_list);
      rack_copy(rack_placeholder, player_get_rack(player_on_turn));
      play_move(best_play, game, NULL);
      sim_results_increment_node_count(sim_results);
      if (ply == plies - 2 || ply == plies - 1) {
        double this_leftover = get_leave_value_for_move(
            player_get_klv(player_on_turn), best_play, rack_placeholder);
        if (player_on_turn_index == simmer->initial_player) {
          leftover += this_leftover;
        } else {
          leftover -= this_leftover;
        }
      }
      simmed_play_add_score_stat(simmed_play, move_get_score(best_play),
                                 move_get_tiles_played(best_play) == RACK_SIZE,
                                 ply, is_multithreaded(simmer));
    }

    int spread =
        player_get_score(game_get_player(game, simmer->initial_player)) -
        player_get_score(game_get_player(game, 1 - simmer->initial_player));
    simmed_play_add_equity_stat(simmed_play, simmer->initial_spread, spread,
                                leftover, is_multithreaded(simmer));
    simmed_play_add_win_pct_stat(
        simmer->win_pcts, simmed_play, spread, leftover,
        game_get_game_end_reason(game),
        // number of tiles unseen to us: bag tiles + tiles on opp rack.
        bag_get_tiles(bag) +
            rack_get_total_letters(player_get_rack(
                game_get_player(game, 1 - simmer->initial_player))),
        plies % 2, is_multithreaded(simmer));
    // reset to first state. we only need to restore one backup.
    game_unplay_last_move(game);
  }
}

void *simmer_worker(void *uncasted_simmer_worker) {
  SimmerWorker *simmer_worker = (SimmerWorker *)uncasted_simmer_worker;
  Simmer *simmer = simmer_worker->simmer;
  SimResults *sim_results = simmer->sim_results;
  ThreadControl *thread_control = simmer->thread_control;
  while (!thread_control_get_is_halted(thread_control)) {
    int current_iteration_count;
    bool reached_max_iteration = false;
    if (is_multithreaded(simmer)) {
      pthread_mutex_lock(&simmer->iteration_count_mutex);
    }
    if (sim_results_get_iteration_count(sim_results) ==
        simmer->max_iterations) {
      reached_max_iteration = true;
    } else {
      sim_results_increment_iteration_count(sim_results);
      current_iteration_count = sim_results_get_iteration_count(sim_results);
    }
    if (is_multithreaded(simmer)) {
      pthread_mutex_unlock(&simmer->iteration_count_mutex);
    }

    if (reached_max_iteration) {
      thread_control_halt(thread_control, HALT_STATUS_MAX_ITERATIONS);
      break;
    }
    sim_single_iteration(simmer_worker);

    int print_info_interval =
        thread_control_get_print_info_interval(thread_control);
    if (print_info_interval > 0 && current_iteration_count > 0 &&
        current_iteration_count % print_info_interval == 0) {
      print_ucgi_sim_stats(simmer_worker->game, simmer->sim_results,
                           thread_control, false);
    }

    int check_stopping_condition_interval =
        thread_control_get_check_stop_interval(thread_control);
    if (check_stopping_condition_interval > 0 &&
        current_iteration_count % check_stopping_condition_interval == 0 &&
        thread_control_set_check_stop_active(thread_control)) {
      if (!thread_control_get_is_halted(thread_control) &&
          handle_potential_stopping_condition(simmer)) {
        thread_control_halt(thread_control, HALT_STATUS_PROBABILISTIC);
      }
      thread_control_set_check_stop_inactive(thread_control);
    }
  }
  log_trace("thread %d exiting", simmer_worker->thread_index);
  return NULL;
}

sim_status_t simulate_internal(const SimArgs *args, Game *game,
                               SimResults *sim_results) {
  if (!args->move_list) {
    return SIM_STATUS_NO_MOVES;
  }

  int move_list_count = move_list_get_count(args->move_list);

  if (move_list_count == 0) {
    return SIM_STATUS_NO_MOVES;
  }

  Simmer *simmer = create_simmer(args, game, move_list_count, sim_results);

  SimmerWorker **simmer_workers =
      malloc_or_die((sizeof(SimmerWorker *)) * (simmer->threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (simmer->threads));

  for (int thread_index = 0; thread_index < simmer->threads; thread_index++) {
    simmer_workers[thread_index] =
        create_simmer_worker(game, simmer, thread_index);
    pthread_create(&worker_ids[thread_index], NULL, simmer_worker,
                   simmer_workers[thread_index]);
  }

  for (int thread_index = 0; thread_index < simmer->threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    destroy_simmer_worker(simmer_workers[thread_index]);
  }

  // Destroy intrasim structs
  free(simmer_workers);
  free(worker_ids);
  destroy_simmer(simmer);

  // Print out the stats
  print_ucgi_sim_stats(game, sim_results, args->thread_control, true);
  return SIM_STATUS_SUCCESS;
}

sim_status_t simulate(const SimArgs *args, SimResults *sim_results) {
  ThreadControl *thread_control = args->thread_control;
  thread_control_unhalt(thread_control);

  Timer *timer = thread_control_get_timer(thread_control);
  mtimer_start(timer);

  Game *game = game_duplicate(args->game);

  sim_status_t sim_status = simulate_internal(args, game, sim_results);

  game_destroy(game);
  gen_destroy_cache();

  mtimer_stop(timer);

  return sim_status;
}