#include "autoplay.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/autoplay_defs.h"
#include "../def/game_defs.h"
#include "../def/thread_control_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"

#include "gameplay.h"
#include "move_gen.h"

#include "../str/autoplay_string.h"

#include "../util/util.h"

typedef struct AutoplayWorker {
  const AutoplayArgs *args;
  AutoplayResults *autoplay_results;
  int max_games_for_worker;
  int worker_index;
} AutoplayWorker;

AutoplayWorker *autoplay_worker_create(const AutoplayArgs *args,
                                       int max_games_for_worker,
                                       int worker_index) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->args = args;
  autoplay_worker->max_games_for_worker = max_games_for_worker;
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->autoplay_results = autoplay_results_create();
  return autoplay_worker;
}

void autoplay_worker_destroy(AutoplayWorker *autoplay_worker) {
  if (!autoplay_worker) {
    return;
  }
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
                        int starting_player_index, int thread_index,
                        uint64_t seed) {
  game_seed(game, seed);
  game_reset(game);
  game_set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  while (!game_over(game)) {
    play_move(get_top_equity_move(game, thread_index, move_list), game, NULL);
  }
  record_results(game, autoplay_results, starting_player_index);
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;

  const AutoplayArgs *args = autoplay_worker->args;
  ThreadControl *thread_control = args->thread_control;
  Game *game = game_create(args->game_args);
  MoveList *move_list = move_list_create(1);

  // Declare local vars for autoplay_worker fields for convenience
  bool use_game_pairs = args->use_game_pairs;
  int max_games_for_worker = autoplay_worker->max_games_for_worker;
  int worker_index = autoplay_worker->worker_index;
  ThreadControlIterOutput iter_output;

  for (int i = 0; i < max_games_for_worker; i++) {
    if (thread_control_get_is_halted(thread_control)) {
      break;
    }
    thread_control_get_next_iter_output(thread_control, &iter_output);
    int starting_player_index = iter_output.iter_count % 2;
    play_autoplay_game(game, move_list, autoplay_worker->autoplay_results,
                       starting_player_index, worker_index, iter_output.seed);
    if (use_game_pairs) {
      play_autoplay_game(game, move_list, autoplay_worker->autoplay_results,
                         1 - starting_player_index, worker_index,
                         iter_output.seed);
    }
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

autoplay_status_t autoplay(const AutoplayArgs *args,
                           AutoplayResults *autoplay_results) {
  ThreadControl *thread_control = args->thread_control;
  int max_iterations = args->max_iterations;

  thread_control_unhalt(thread_control);
  thread_control_reset_iter_count(thread_control);
  autoplay_results_reset(autoplay_results);

  int number_of_threads = thread_control_get_threads(thread_control);
  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {

    int number_of_games_for_worker = get_number_of_games_for_worker(
        max_iterations, number_of_threads, thread_index);

    autoplay_workers[thread_index] =
        autoplay_worker_create(args, number_of_games_for_worker, thread_index);

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
    autoplay_worker_destroy(autoplay_workers[thread_index]);
  }

  // Destroy intrasim structs
  free(autoplay_workers);
  free(worker_ids);

  print_ucgi_autoplay_results(autoplay_results, thread_control);
  gen_destroy_cache();

  return AUTOPLAY_STATUS_SUCCESS;
}
