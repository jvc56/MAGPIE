#include "simmer.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "../def/rack_defs.h"
#include "../def/stats_defs.h"

#include "../util/log.h"
#include "../util/util.h"

#include "../str/game_string.h"
#include "../str/sim_string.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/timer.h"
#include "../ent/win_pct.h"

#include "gameplay.h"
#include "move_gen.h"

#define MAX_STOPPING_ITERATION_CT 4000
#define PER_PLY_STOPPING_SCALING 1250
#define SIMILAR_PLAYS_ITER_CUTOFF 1000

typedef struct Simmer {
  int initial_spread;
  int initial_player;
  int max_iterations;
  int stopping_condition;
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

Simmer *create_simmer(const Config *config, Game *game, MoveList *move_list,
                      int num_simmed_plays, SimResults *sim_results) {
  Simmer *simmer = malloc_or_die(sizeof(Simmer));
  ThreadControl *thread_control = config_get_thread_control(config);
  int ld_size =
      letter_distribution_get_size(config_get_letter_distribution(config));

  simmer->initial_player = game_get_player_on_turn_index(game);
  simmer->initial_spread =
      player_get_score(game_get_player(game, simmer->initial_player)) -
      player_get_score(game_get_player(game, 1 - simmer->initial_player));
  simmer->max_iterations = config_get_max_iterations(config);
  simmer->stopping_condition = config_get_stopping_condition(config);
  simmer->threads = get_number_of_threads(thread_control);
  simmer->seed = config_get_seed(config);
  pthread_mutex_init(&simmer->iteration_count_mutex, NULL);

  Rack *opponent_known_tiles = config_get_rack(config);
  if (opponent_known_tiles) {
    simmer->known_opp_rack = rack_duplicate(opponent_known_tiles);
  } else {
    simmer->known_opp_rack = NULL;
  }

  simmer->similar_plays_rack = create_rack(ld_size);

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

  simmer->win_pcts = config_get_win_pcts(config);

  simmer->thread_control = thread_control;

  sim_results_reset(sim_results, move_list, num_simmed_plays,
                    config_get_plies(config));

  simmer->sim_results = sim_results;

  return simmer;
}

void destroy_simmer(Simmer *simmer) {
  if (simmer->similar_plays_rack) {
    destroy_rack(simmer->similar_plays_rack);
  }
  if (simmer->known_opp_rack) {
    destroy_rack(simmer->known_opp_rack);
  }
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
  set_backup_mode(simmer_worker->game, BACKUP_MODE_SIMULATION);
  seed_bag_for_worker(game_get_bag(simmer_worker->game), simmer->seed,
                      worker_index);

  // MoveList
  simmer_worker->move_list = create_move_list(1);

  int ld_size = letter_distribution_get_size(game_get_ld(simmer_worker->game));

  // Rack placeholder
  simmer_worker->rack_placeholder = create_rack(ld_size);

  // Simmer
  simmer_worker->simmer = simmer;

  return simmer_worker;
}

void destroy_simmer_worker(SimmerWorker *simmer_worker) {
  destroy_game(simmer_worker->game);
  destroy_move_list(simmer_worker->move_list);
  destroy_rack(simmer_worker->rack_placeholder);
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

  if (get_move_type(m1_move) != get_move_type(m2_move)) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }

  // Otherwise, we must compute play similarity and fill in the cache.
  // two plays are "similar" if they use the same tiles, and they start at
  // the same square.
  if (!(get_dir(m1_move) == get_dir(m2_move) &&
        get_col_start(m1_move) == get_col_start(m2_move) &&
        get_row_start(m1_move) == get_row_start(m2_move))) {

    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }
  if (!(move_get_tiles_played(m1_move) == move_get_tiles_played(m2_move) &&
        get_tiles_length(m1_move) == get_tiles_length(m2_move))) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }

  // Create a rack from m1, then subtract the rack from m2. The final
  // rack should have all zeroes.
  reset_rack(simmer->similar_plays_rack);
  for (int i = 0; i < get_tiles_length(m1_move); i++) {
    uint8_t tile = get_tile(m1_move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (is_blanked(ml)) {
      ml = 0;
    }
    add_letter_to_rack(simmer->similar_plays_rack, ml);
  }

  for (int i = 0; i < get_tiles_length(m2_move); i++) {
    uint8_t tile = get_tile(m1_move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (is_blanked(ml)) {
      ml = 0;
    }
    take_letter_from_rack(simmer->similar_plays_rack, ml);
  }

  for (int i = 0; i < get_array_size(simmer->similar_plays_rack); i++) {
    if (get_number_of_letter(simmer->similar_plays_rack, i) != 0) {
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

  double zval = 0;
  bool use_stopping_cond = true;
  switch (simmer->stopping_condition) {
  case SIM_STOPPING_CONDITION_NONE:
    use_stopping_cond = false;
    break;
  case SIM_STOPPING_CONDITION_95PCT:
    zval = STATS_Z95;
    break;
  case SIM_STOPPING_CONDITION_98PCT:
    zval = STATS_Z98;
    break;
  case SIM_STOPPING_CONDITION_99PCT:
    zval = STATS_Z99;
    break;
  }

  SimResults *sim_results = simmer->sim_results;
  int number_of_plays = sim_results_get_number_of_plays(sim_results);
  int total_ignored = 0;

  sim_results_lock_simmed_plays(simmer->sim_results);
  sim_results_sort_plays_by_win_rate(simmer->sim_results);

  const SimmedPlay *tentative_winner =
      sim_results_get_simmed_play(sim_results, 0);
  double mu = get_mean(simmed_play_get_win_pct_stat(tentative_winner));
  double std_error =
      get_standard_error(simmed_play_get_win_pct_stat(tentative_winner), zval);

  for (int i = 1; i < number_of_plays; i++) {
    SimmedPlay *simmed_play = sim_results_get_simmed_play(sim_results, i);
    if (simmed_play_get_ignore(simmed_play)) {
      total_ignored++;
      continue;
    }
    double mu_i = get_mean(simmed_play_get_win_pct_stat(simmed_play));
    double std_error_i =
        get_standard_error(simmed_play_get_win_pct_stat(simmed_play), zval);

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

  // set random rack for opponent (throw in rack, shuffle, draw new tiles).
  set_random_rack(game, 1 - game_get_player_on_turn_index(game),
                  simmer->known_opp_rack);
  // need a new shuffle for every iteration:
  Bag *bag = game_get_bag(game);
  shuffle(bag);

  for (int i = 0; i < number_of_plays; i++) {
    SimmedPlay *simmed_play = sim_results_get_simmed_play(sim_results, i);
    if (simmed_play_get_ignore(simmed_play)) {
      continue;
    }

    double leftover = 0.0;
    set_backup_mode(game, BACKUP_MODE_SIMULATION);
    // play move
    play_move(simmed_play_get_move(simmed_play), game);
    sim_results_increment_node_count(sim_results);
    set_backup_mode(game, BACKUP_MODE_OFF);
    // further plies will NOT be backed up.
    for (int ply = 0; ply < plies; ply++) {
      int player_on_turn_index = game_get_player_on_turn_index(game);
      Player *player_on_turn = game_get_player(game, player_on_turn_index);
      game_end_reason_t game_end_reason = game_get_game_end_reason(game);

      if (game_end_reason != GAME_END_REASON_NONE) {
        // game is over.
        break;
      }

      const Move *best_play = get_top_equity_move(
          game, simmer_worker->thread_index, simmer_worker->move_list);
      rack_copy(rack_placeholder, player_get_rack(player_on_turn));
      play_move(best_play, game);
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
      add_score_stat(simmed_play, get_score(best_play),
                     move_get_tiles_played(best_play) == RACK_SIZE, ply,
                     is_multithreaded(simmer));
    }

    int spread =
        player_get_score(game_get_player(game, simmer->initial_player)) -
        player_get_score(game_get_player(game, 1 - simmer->initial_player));
    add_equity_stat(simmed_play, simmer->initial_spread, spread, leftover,
                    is_multithreaded(simmer));
    add_win_pct_stat(
        simmer->win_pcts, simmed_play, spread, leftover,
        game_get_game_end_reason(game),
        // number of tiles unseen to us: bag tiles + tiles on opp rack.
        get_tiles_remaining(bag) +
            get_number_of_letters(player_get_rack(
                game_get_player(game, 1 - simmer->initial_player))),
        plies % 2, is_multithreaded(simmer));
    // reset to first state. we only need to restore one backup.
    unplay_last_move(game);
  }
}

void *simmer_worker(void *uncasted_simmer_worker) {
  SimmerWorker *simmer_worker = (SimmerWorker *)uncasted_simmer_worker;
  Simmer *simmer = simmer_worker->simmer;
  SimResults *sim_results = simmer->sim_results;
  ThreadControl *thread_control = simmer->thread_control;
  while (!is_halted(thread_control)) {
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
      halt(thread_control, HALT_STATUS_MAX_ITERATIONS);
      break;
    }
    sim_single_iteration(simmer_worker);

    int print_info_interval = get_print_info_interval(thread_control);
    if (print_info_interval > 0 && current_iteration_count > 0 &&
        current_iteration_count % print_info_interval == 0) {
      print_ucgi_sim_stats(simmer_worker->game, simmer->sim_results,
                           thread_control, 0);
    }

    int check_stopping_condition_interval =
        get_check_stopping_condition_interval(thread_control);
    if (check_stopping_condition_interval > 0 &&
        current_iteration_count % check_stopping_condition_interval == 0 &&
        set_check_stop_active(thread_control)) {
      if (!is_halted(thread_control) &&
          handle_potential_stopping_condition(simmer)) {
        halt(thread_control, HALT_STATUS_PROBABILISTIC);
      }
      set_check_stop_inactive(thread_control);
    }
  }
  log_trace("thread %d exiting", simmer_worker->thread_index);
  return NULL;
}

void simmer_sort_plays_by_win_rate(Simmer *simmer) {
  sim_results_lock_simmed_plays(simmer->sim_results);
  sim_results_sort_plays_by_win_rate(simmer->sim_results);
  sim_results_unlock_simmed_plays(simmer->sim_results);
}

sim_status_t simulate_with_game_copy(const Config *config, Game *game,
                                     SimResults *sim_results) {
  ThreadControl *thread_control = config_get_thread_control(config);
  unhalt(thread_control);

  int num_simmed_plays = config_get_num_plays(config);

  MoveList *move_list = create_move_list(num_simmed_plays);
  generate_moves(game, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0, move_list);
  // Sorting moves converts the move list from a min heap
  // to a sorted array with count == 0, so the number of
  // moves must be obtained before sorting.
  int number_of_moves_generated = move_list_get_count(move_list);
  sort_moves(move_list);

  if (config_get_static_search_only(config)) {
    print_ucgi_static_moves(game, move_list, thread_control);
    return SIM_STATUS_SUCCESS;
  }

  if (number_of_moves_generated < num_simmed_plays) {
    num_simmed_plays = number_of_moves_generated;
  }

  if (num_simmed_plays == 0) {
    return SIM_STATUS_SUCCESS;
  }

  Simmer *simmer =
      create_simmer(config, game, move_list, num_simmed_plays, sim_results);

  destroy_move_list(move_list);

  SimmerWorker **simmer_workers =
      malloc_or_die((sizeof(SimmerWorker *)) * (simmer->threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (simmer->threads));

  Timer *timer = get_timer(thread_control);
  mtimer_start(timer);

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
  print_ucgi_sim_stats(game, sim_results, thread_control, true);
  return SIM_STATUS_SUCCESS;
}

sim_status_t simulate(const Config *config, const Game *input_game,
                      SimResults *sim_results) {
  Game *game = game_duplicate(input_game);
  sim_status_t sim_status = simulate_with_game_copy(config, game, sim_results);
  destroy_game(game);
  gen_clear_cache();
  return sim_status;
}