#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "../def/autoplay_defs.h"
#include "../def/rack_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/player.h"
#include "../ent/stats.h"

#include "gameplay.h"
#include "move_gen.h"

#include "../str/autoplay_string.h"

#include "../util/util.h"

typedef struct AutoplayWorker {
  const Config *config;
  AutoplayResults *autoplay_results;
  int max_games_for_worker;
  int worker_index;
  uint64_t seed;
} AutoplayWorker;

AutoplayWorker *create_autoplay_worker(const Config *config,
                                       int max_games_for_worker,
                                       int worker_index, uint64_t seed) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->config = config;
  autoplay_worker->max_games_for_worker = max_games_for_worker;
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->seed = seed;
  autoplay_worker->autoplay_results = autoplay_results_create();
  return autoplay_worker;
}

void destroy_autoplay_worker(AutoplayWorker *autoplay_worker) {
  autoplay_results_destroy(autoplay_worker->autoplay_results);
  free(autoplay_worker);
}

void record_results(Game *game, AutoplayResults *autoplay_results,
                    int starting_player_index) {

  int p0_score = player_get_score(game_get_player(game, 0));
  int p1_score = player_get_score(game_get_player(game, 1));

  autoplay_results_increment_total_games(autoplay_results);
  if (p0_score > p1_score) {
    autoplay_results_increment_p1_wins(autoplay_results);
  } else if (p1_score > p0_score) {
    autoplay_results_increment_p1_losses(autoplay_results);
  } else {
    autoplay_results_increment_p1_ties(autoplay_results);
  }
  if (starting_player_index == 0) {
    autoplay_results_increment_p1_firsts(autoplay_results);
  }
  autoplay_results_increment_p1_score(autoplay_results, p0_score);
  autoplay_results_increment_p2_score(autoplay_results, p1_score);
}

void play_autoplay_game(Game *game, MoveList *move_list,
                        AutoplayResults *autoplay_results,
                        int starting_player_index, int thread_index) {
  game_reset(game);
  game_set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    play_move(get_top_equity_move(game, thread_index, move_list), game);
  }
  record_results(game, autoplay_results, starting_player_index);
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;
  const Config *config = autoplay_worker->config;
  ThreadControl *thread_control = config_get_thread_control(config);
  Game *game = game_create(config);
  Bag *bag = game_get_bag(game);
  MoveList *move_list = move_list_create(1);

  // Declare local vars for autoplay_worker fields for convenience
  bool use_game_pairs = config_get_use_game_pairs(autoplay_worker->config);
  int worker_index = autoplay_worker->worker_index;
  int starting_player_for_thread = worker_index % 2;

  Bag *game_pair_bag;
  if (use_game_pairs) {
    // Create a Bag to save the PRNG state of the game
    // to use for game pairs. The initial seed does
    // not matter since it will be overwritten before
    // the first game of the pair is played.
    game_pair_bag = bag_create(game_get_ld(game));
  }
  bag_seed_for_worker(bag, autoplay_worker->seed, worker_index);

  for (int i = 0; i < autoplay_worker->max_games_for_worker; i++) {
    if (thread_control_get_is_halted(thread_control)) {
      break;
    }
    int starting_player_index = (i + starting_player_for_thread) % 2;

    // If we are using game pairs, we have to save the state of the
    // Bag PRNG before playing the first game so the state can be
    // reloaded before playing the second game of the pair, ensuring
    // both games are played with an identical Bag PRNG.
    if (use_game_pairs) {
      bag_copy(game_pair_bag, bag);
    }

    play_autoplay_game(game, move_list, autoplay_worker->autoplay_results,
                       starting_player_index, autoplay_worker->worker_index);
    if (use_game_pairs) {
      bag_copy(bag, game_pair_bag);
      play_autoplay_game(game, move_list, autoplay_worker->autoplay_results,
                         1 - starting_player_index,
                         autoplay_worker->worker_index);
    }
  }

  if (use_game_pairs) {
    bag_destroy(game_pair_bag);
  }
  move_list_destroy(move_list);
  game_destroy(game);
  return NULL;
}

int get_number_of_games_for_worker(int max_iterations, int number_of_threads,
                                   int thread_index) {
  int number_of_games_for_worker = (max_iterations / number_of_threads);
  if (max_iterations % number_of_threads > thread_index) {
    number_of_games_for_worker++;
  }
  return number_of_games_for_worker;
}

autoplay_status_t autoplay(const Config *config,
                           AutoplayResults *autoplay_results) {
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_unhalt(thread_control);
  autoplay_results_reset(autoplay_results);

  int number_of_threads = thread_control_get_threads(thread_control);
  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {

    int number_of_games_for_worker = get_number_of_games_for_worker(
        config_get_max_iterations(config), number_of_threads, thread_index);

    autoplay_workers[thread_index] =
        create_autoplay_worker(config, number_of_games_for_worker, thread_index,
                               config_get_seed(config));

    pthread_create(&worker_ids[thread_index], NULL, autoplay_worker,
                   autoplay_workers[thread_index]);
  }

  Stat **p1_score_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
  Stat **p2_score_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    autoplay_results_add(autoplay_workers[thread_index]->autoplay_results,
                         autoplay_results);
    p1_score_stats[thread_index] = autoplay_results_get_p1_score(
        autoplay_workers[thread_index]->autoplay_results);
    p2_score_stats[thread_index] = autoplay_results_get_p2_score(
        autoplay_workers[thread_index]->autoplay_results);
  }

  // If autoplay was interrupted by the user,
  // this will not change the status.
  thread_control_halt(thread_control, HALT_STATUS_MAX_ITERATIONS);

  stats_combine(p1_score_stats, number_of_threads,
                autoplay_results_get_p1_score(autoplay_results));
  free(p1_score_stats);

  stats_combine(p2_score_stats, number_of_threads,
                autoplay_results_get_p2_score(autoplay_results));
  free(p2_score_stats);

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    destroy_autoplay_worker(autoplay_workers[thread_index]);
  }

  // Destroy intrasim structs
  free(autoplay_workers);
  free(worker_ids);

  print_ucgi_autoplay_results(autoplay_results, thread_control);
  gen_clear_cache();

  return AUTOPLAY_STATUS_SUCCESS;
}
