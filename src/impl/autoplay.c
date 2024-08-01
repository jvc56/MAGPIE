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
#include "../ent/checkpoint.h"
#include "../ent/game.h"
#include "../ent/leave_list.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/xoshiro.h"

#include "gameplay.h"
#include "move_gen.h"

#include "../util/util.h"

typedef struct SharedData {
  KLV *klv;
  LeaveList *leave_list;
  pthread_mutex_t leave_list_mutex;
  Checkpoint *checkpoint;
} SharedData;

void leave_gen_prebroadcast_func(void *data) {
  SharedData *shared_data = (SharedData *)data;
  leave_list_write_to_klv(shared_data->leave_list);
}

typedef struct AutoplayWorker {
  const AutoplayArgs *args;
  AutoplayResults *autoplay_results;
  int max_games_for_worker;
  int worker_index;
  SharedData *shared_data;
} AutoplayWorker;

AutoplayWorker *autoplay_worker_create(const AutoplayArgs *args,
                                       const AutoplayResults *target,
                                       int max_games_for_worker,
                                       int worker_index,
                                       SharedData *shared_data) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->args = args;
  autoplay_worker->max_games_for_worker = max_games_for_worker;
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->autoplay_results =
      autoplay_results_create_empty_copy(target);
  autoplay_worker->shared_data = shared_data;
  return autoplay_worker;
}

void autoplay_worker_destroy(AutoplayWorker *autoplay_worker) {
  if (!autoplay_worker) {
    return;
  }
  autoplay_results_destroy(autoplay_worker->autoplay_results);
  free(autoplay_worker);
}

SharedData *autoplay_worker_shared_data_create(const LetterDistribution *ld,
                                               KLV *klv,
                                               int number_of_threads) {
  SharedData *shared_data = malloc_or_die(sizeof(SharedData));
  shared_data->klv = klv;
  shared_data->leave_list = leave_list_create(ld, klv);
  shared_data->checkpoint =
      checkpoint_create(number_of_threads, leave_gen_prebroadcast_func);
  pthread_mutex_init(&shared_data->leave_list_mutex, NULL);
  return shared_data;
}

void autoplay_worker_shared_data_destroy(SharedData *shared_data) {
  if (!shared_data) {
    return;
  }
  leave_list_destroy(shared_data->leave_list);
  free(shared_data);
}

void autoplay_leave_list_draw_rarest_available_leave(
    AutoplayWorker *autoplay_worker, Game *game, int player_on_turn_index) {
  pthread_mutex_lock(&autoplay_worker->shared_data->leave_list_mutex);
  leave_list_draw_rarest_available_leave(
      autoplay_worker->shared_data->leave_list, game_get_bag(game),
      player_get_rack(game_get_player(game, player_on_turn_index)),
      game_get_player_draw_index(game, player_on_turn_index));
  pthread_mutex_unlock(&autoplay_worker->shared_data->leave_list_mutex);
}

void autoplay_leave_list_add_leave(AutoplayWorker *autoplay_worker, Game *game,
                                   int player_on_turn_index,
                                   double move_equity) {
  pthread_mutex_lock(&autoplay_worker->shared_data->leave_list_mutex);
  leave_list_add_leave(
      autoplay_worker->shared_data->leave_list,
      autoplay_worker->shared_data->klv,
      player_get_rack(game_get_player(game, player_on_turn_index)),
      move_equity);
  pthread_mutex_unlock(&autoplay_worker->shared_data->leave_list_mutex);
}

void play_autoplay_game(AutoplayWorker *autoplay_worker, Game *game,
                        MoveList *move_list, Rack *leaves[2],
                        int starting_player_index, uint64_t seed) {
  AutoplayResults *autoplay_results = autoplay_worker->autoplay_results;
  const int thread_index = autoplay_worker->worker_index;
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  int turn_number = 0;
  int force_draw_turn_number = -1;
  if (leaves[0] && leaves[1]) {
    force_draw_turn_number =
        prng_get_random_number(bag_get_prng(game_get_bag(game)),
                               autoplay_worker->args->max_force_draw_turn);
  }
  while (!game_over(game)) {
    const int player_on_turn_index = game_get_player_on_turn_index(game);
    Rack *leave_from_previous_move = leaves[player_on_turn_index];
    if (turn_number == force_draw_turn_number) {
      return_rack_to_bag(game, player_on_turn_index);
      draw_rack_from_bag(game, player_on_turn_index, leave_from_previous_move);
      autoplay_leave_list_draw_rarest_available_leave(autoplay_worker, game,
                                                      player_on_turn_index);
      draw_to_full_rack(game, player_on_turn_index);
      // FIXME: end the game after recording this move.
    }
    const Move *move = get_top_equity_move(game, thread_index, move_list);
    autoplay_results_add_move(autoplay_results, move);
    if (force_draw_turn_number > 0) {
      autoplay_leave_list_add_leave(autoplay_worker, game, player_on_turn_index,
                                    move_get_equity(move));
    }
    play_move(move, game, NULL, leave_from_previous_move);
    turn_number++;
  }
  autoplay_results_add_game(autoplay_results, game);
}

void autoplay_single_generation(AutoplayWorker *autoplay_worker, Game *game,
                                MoveList *move_list, Rack *leaves[2]) {
  const AutoplayArgs *args = autoplay_worker->args;
  ThreadControl *thread_control = args->thread_control;

  const bool use_game_pairs =
      args->use_game_pairs && args->type == AUTOPLAY_TYPE_DEFAULT;
  const int max_games_for_worker = autoplay_worker->max_games_for_worker;
  ThreadControlIterOutput iter_output;

  for (int j = 0; j < max_games_for_worker; j++) {
    if (thread_control_get_is_halted(thread_control)) {
      break;
    }
    thread_control_get_next_iter_output(thread_control, &iter_output);
    int starting_player_index = iter_output.iter_count % 2;
    play_autoplay_game(autoplay_worker, game, move_list, leaves,
                       starting_player_index, iter_output.seed);
    if (use_game_pairs) {
      play_autoplay_game(autoplay_worker, game, move_list, leaves,
                         1 - starting_player_index, iter_output.seed);
    }
  }
}

void autoplay_leave_gen(AutoplayWorker *autoplay_worker, Game *game,
                        MoveList *move_list) {
  const AutoplayArgs *args = autoplay_worker->args;
  const int gens = args->gens;
  const LetterDistribution *ld = args->game_args->ld;

  Rack *leaves[2] = {rack_create(ld_get_size(ld)),
                     rack_create(ld_get_size(ld))};

  for (int i = 0; i < gens; i++) {
    autoplay_single_generation(autoplay_worker, game, move_list, leaves);
    checkpoint_wait(autoplay_worker->shared_data->checkpoint,
                    autoplay_worker->shared_data);
  }

  rack_destroy(leaves[0]);
  rack_destroy(leaves[1]);
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;
  const AutoplayArgs *args = autoplay_worker->args;
  Game *game = game_create(args->game_args);
  MoveList *move_list = move_list_create(1);

  switch (args->type) {
  case AUTOPLAY_TYPE_DEFAULT:
    autoplay_single_generation(autoplay_worker, game, move_list,
                               (Rack *[2]){NULL, NULL});
    break;
  case AUTOPLAY_TYPE_LEAVE_GEN:
    autoplay_leave_gen(autoplay_worker, game, move_list);
    break;
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

  thread_control_unhalt(thread_control);
  thread_control_reset_iter_count(thread_control);
  autoplay_results_reset(autoplay_results);

  const int max_iterations = args->max_iterations;
  const int number_of_threads = thread_control_get_threads(thread_control);

  // We can use player index 0 here since it is guaranteed that
  // players share the the KLV.
  SharedData *shared_data = NULL;
  if (args->type == AUTOPLAY_TYPE_LEAVE_GEN) {
    KLV *klv = players_data_get_klv(args->game_args->players_data, 0);
    shared_data = autoplay_worker_shared_data_create(args->game_args->ld, klv,
                                                     number_of_threads);
  }

  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    int number_of_games_for_worker = get_number_of_games_for_worker(
        max_iterations, number_of_threads, thread_index);
    autoplay_workers[thread_index] = autoplay_worker_create(
        args, autoplay_results, number_of_games_for_worker, thread_index,
        shared_data);
    pthread_create(&worker_ids[thread_index], NULL, autoplay_worker,
                   autoplay_workers[thread_index]);
  }

  AutoplayResults **autoplay_results_list =
      malloc_or_die((sizeof(AutoplayResults *)) * (number_of_threads));

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    autoplay_results_list[thread_index] =
        autoplay_workers[thread_index]->autoplay_results;
  }

  // If autoplay was interrupted by the user,
  // this will not change the status.
  thread_control_halt(thread_control, HALT_STATUS_MAX_ITERATIONS);

  autoplay_results_combine(autoplay_results_list, number_of_threads,
                           autoplay_results);

  free(autoplay_results_list);

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    autoplay_worker_destroy(autoplay_workers[thread_index]);
  }

  // Destroy intrasim structs
  free(autoplay_workers);
  free(worker_ids);

  char *autoplay_results_string =
      autoplay_results_to_string(autoplay_results, false);
  thread_control_print(thread_control, autoplay_results_string);
  free(autoplay_results_string);
  gen_destroy_cache();

  return AUTOPLAY_STATUS_SUCCESS;
}
