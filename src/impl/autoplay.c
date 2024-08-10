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
#include "../ent/klv.h"
#include "../ent/leave_list.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/xoshiro.h"

#include "gameplay.h"
#include "move_gen.h"

#include "../util/string_util.h"
#include "../util/util.h"

#define MAX_RECORDED_FORCE_DRAW_TURNS 50

typedef struct SharedData {
  int gens_completed;
  int games_per_gen;
  const LetterDistribution *ld;
  const char *data_paths;
  KLV *klv;
  LeaveList *leave_list;
  pthread_mutex_t leave_list_mutex;
  Checkpoint *checkpoint;
  int forced_draw_turns_count[MAX_RECORDED_FORCE_DRAW_TURNS];
  Stat *force_draw_turns;
} SharedData;

void leave_gen_prebroadcast_func(void *data) {
  SharedData *shared_data = (SharedData *)data;
  leave_list_write_to_klv(shared_data->leave_list);
  shared_data->gens_completed++;
  // Write the KLV for the current generation.
  char *label = get_formatted_string("_gen_%d", shared_data->gens_completed);
  char *gen_labeled_klv_name = insert_before_dot(shared_data->klv->name, label);
  char *gen_labeled_klv_filename = data_filepaths_get_writable_filename(
      shared_data->data_paths, gen_labeled_klv_name, DATA_FILEPATH_TYPE_KLV);
  klv_write(shared_data->klv, gen_labeled_klv_name);
  free(gen_labeled_klv_name);
  free(label);
  // Print info about the current state.
  StringBuilder *leave_gen_sb = string_builder_create();
  string_builder_add_formatted_string(leave_gen_sb, "Games: %d\n",
                                      shared_data->gens_completed *
                                          shared_data->games_per_gen);
  string_builder_add_formatted_string(
      leave_gen_sb, "Moves: %d\n\n",
      leave_list_get_empty_leave_count(shared_data->leave_list));
  string_builder_add_most_or_least_common_leaves(
      leave_gen_sb, shared_data->leave_list, shared_data->ld, 20, true);
  string_builder_add_char(leave_gen_sb, '\n');
  string_builder_add_most_or_least_common_leaves(
      leave_gen_sb, shared_data->leave_list, shared_data->ld, 20, false);
  string_builder_add_formatted_string(
      leave_gen_sb, "\nForced draw average turn: %d\n\nForced draw counts:\n\n",
      stat_get_mean(shared_data->force_draw_turns));
  for (int i = 0; i < MAX_RECORDED_FORCE_DRAW_TURNS; i++) {
    string_builder_add_formatted_string(
        leave_gen_sb, "%d: %d\n", i + 1,
        shared_data->forced_draw_turns_count[i]);
  }

  char *report_name_prefix = cut_off_after_char(gen_labeled_klv_filename, '.');
  char *report_name =
      get_formatted_string("%s%s.txt", report_name_prefix, label, ".txt");

  write_string_to_file(report_name, "w", string_builder_peek(leave_gen_sb));
  string_builder_destroy(leave_gen_sb);

  free(report_name);
  free(report_name_prefix);
  free(gen_labeled_klv_filename);
  free(gen_labeled_klv_name);
  free(label);
}

typedef struct AutoplayWorker {
  const AutoplayArgs *args;
  AutoplayResults *autoplay_results;
  int worker_index;
  SharedData *shared_data;
} AutoplayWorker;

AutoplayWorker *autoplay_worker_create(const AutoplayArgs *args,
                                       const AutoplayResults *target,
                                       int worker_index,
                                       SharedData *shared_data) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->args = args;
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

// Use NULL for the KLV when not running in leave gen mode.
// If the KLV is NULL, the only the gens_completed and games_per_gen fields
// are set, since all autoplay modes use those fields.
// If the KLV is not NULL, then all of the fields are set, since only
// the leave gen mode uses the KLV, leave_list, etc.
SharedData *autoplay_worker_shared_data_create(const LetterDistribution *ld,
                                               const char *data_paths, KLV *klv,
                                               int number_of_threads,
                                               int games_per_gen) {
  SharedData *shared_data = malloc_or_die(sizeof(SharedData));
  shared_data->gens_completed = 0;
  shared_data->games_per_gen = games_per_gen;
  shared_data->klv = NULL;
  shared_data->leave_list = NULL;
  shared_data->checkpoint = NULL;
  shared_data->force_draw_turns = NULL;
  if (klv) {
    shared_data->ld = ld;
    shared_data->data_paths = data_paths;
    shared_data->klv = klv;
    shared_data->leave_list = leave_list_create(ld, klv);
    shared_data->checkpoint =
        checkpoint_create(number_of_threads, leave_gen_prebroadcast_func);
    pthread_mutex_init(&shared_data->leave_list_mutex, NULL);
    for (int i = 0; i < MAX_RECORDED_FORCE_DRAW_TURNS; i++) {
      shared_data->forced_draw_turns_count[i] = 0;
    }
    shared_data->force_draw_turns = stat_create(false);
  }
  return shared_data;
}

void autoplay_worker_shared_data_destroy(SharedData *shared_data) {
  if (!shared_data) {
    return;
  }
  leave_list_destroy(shared_data->leave_list);
  checkpoint_destroy(shared_data->checkpoint);
  stat_destroy(shared_data->force_draw_turns);
  free(shared_data);
}

void autoplay_leave_list_draw_rarest_available_leave(
    AutoplayWorker *autoplay_worker, Game *game, int player_on_turn_index,
    int force_draw_turn) {
  pthread_mutex_lock(&autoplay_worker->shared_data->leave_list_mutex);
  leave_list_draw_rarest_available_leave(
      autoplay_worker->shared_data->leave_list, game_get_bag(game),
      player_get_rack(game_get_player(game, player_on_turn_index)),
      game_get_player_draw_index(game, player_on_turn_index));
  if (force_draw_turn < MAX_RECORDED_FORCE_DRAW_TURNS) {
    autoplay_worker->shared_data->forced_draw_turns_count[force_draw_turn]++;
  }
  stat_push(autoplay_worker->shared_data->force_draw_turns,
            (double)force_draw_turn, 1);
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
  if (leaves[0]) {
    force_draw_turn_number =
        prng_get_random_number(bag_get_prng(game_get_bag(game)),
                               autoplay_worker->args->max_force_draw_turn);
  }
  bool end_game = false;
  while (!game_over(game) && !end_game) {
    const int player_on_turn_index = game_get_player_on_turn_index(game);
    Rack *leave_from_previous_move = leaves[player_on_turn_index];
    const bool leave_gen_add_leave =
        force_draw_turn_number >= 0 &&
        bag_get_tiles(game_get_bag(game)) > (RACK_SIZE);
    if (turn_number == force_draw_turn_number && leave_gen_add_leave) {
      return_rack_to_bag(game, player_on_turn_index);
      draw_rack_from_bag(game, player_on_turn_index, leave_from_previous_move);
      autoplay_leave_list_draw_rarest_available_leave(
          autoplay_worker, game, player_on_turn_index, turn_number);
      draw_to_full_rack(game, player_on_turn_index);
      end_game = true;
    }
    const Move *move = get_top_equity_move(game, thread_index, move_list);
    autoplay_results_add_move(autoplay_results, move);
    if (leave_gen_add_leave) {
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

  const uint64_t stop_count =
      (autoplay_worker->shared_data->gens_completed + 1) *
      autoplay_worker->shared_data->games_per_gen;
  const bool use_game_pairs =
      args->use_game_pairs && args->type == AUTOPLAY_TYPE_DEFAULT;
  ThreadControlIterOutput iter_output;

  while (true) {
    if (thread_control_get_is_halted(thread_control)) {
      break;
    }
    if (thread_control_get_next_iter_output(thread_control, &iter_output,
                                            stop_count)) {
      break;
    }
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
  ThreadControl *thread_control = args->thread_control;
  const int gens = args->gens;
  const LetterDistribution *ld = args->game_args->ld;

  Rack *leaves[2] = {rack_create(ld_get_size(ld)),
                     rack_create(ld_get_size(ld))};

  for (int i = 0; i < gens; i++) {
    autoplay_single_generation(autoplay_worker, game, move_list, leaves);
    checkpoint_wait(autoplay_worker->shared_data->checkpoint,
                    autoplay_worker->shared_data);
    if (thread_control_get_is_halted(thread_control)) {
      break;
    }
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

autoplay_status_t autoplay(const AutoplayArgs *args,
                           AutoplayResults *autoplay_results) {
  ThreadControl *thread_control = args->thread_control;

  thread_control_unhalt(thread_control);
  thread_control_reset_iter_count(thread_control);
  autoplay_results_reset(autoplay_results);

  const int number_of_threads = thread_control_get_threads(thread_control);

  KLV *klv = NULL;
  if (args->type == AUTOPLAY_TYPE_LEAVE_GEN) {
    // We can use player index 0 here since it is guaranteed that
    // players share the the KLV.
    klv = players_data_get_klv(args->game_args->players_data, 0);
  }
  SharedData *shared_data = autoplay_worker_shared_data_create(
      args->game_args->ld, args->data_paths, klv, number_of_threads,
      args->games_per_gen);

  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    autoplay_workers[thread_index] = autoplay_worker_create(
        args, autoplay_results, thread_index, shared_data);
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

  free(autoplay_workers);
  free(worker_ids);
  autoplay_worker_shared_data_destroy(shared_data);

  players_data_reload(args->game_args->players_data, PLAYERS_DATA_TYPE_KLV,
                      args->data_paths);

  char *autoplay_results_string =
      autoplay_results_to_string(autoplay_results, false);
  thread_control_print(thread_control, autoplay_results_string);
  free(autoplay_results_string);
  gen_destroy_cache();

  return AUTOPLAY_STATUS_SUCCESS;
}
