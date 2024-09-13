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
#include "klv_csv.h"
#include "move_gen.h"

#include "../util/string_util.h"
#include "../util/util.h"

typedef struct LeavegenSharedData {
  uint64_t gens_completed;
  uint64_t gen_start_games;
  AutoplayResults *gen_autoplay_results;
  const LetterDistribution *ld;
  const char *data_paths;
  KLV *klv;
  LeaveList *leave_list;
  Checkpoint *postgen_checkpoint;
  AutoplayResults *primary_autoplay_results;
  AutoplayResults **autoplay_results_list;
} LeavegenSharedData;

typedef struct AutoplaySharedData {
  uint64_t games_per_gen;
  ThreadControl *thread_control;
  LeavegenSharedData *leavegen_shared_data;
} AutoplaySharedData;

void postgen_prebroadcast_func(void *data) {
  AutoplaySharedData *shared_data = (AutoplaySharedData *)data;
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  leave_list_write_to_klv(lg_shared_data->leave_list);
  lg_shared_data->gens_completed++;

  // Write the KLV for the current generation.
  char *label = get_formatted_string("_gen_%d", lg_shared_data->gens_completed);
  char *gen_labeled_klv_name =
      insert_before_dot(lg_shared_data->klv->name, label);
  char *gen_labeled_klv_filename = data_filepaths_get_writable_filename(
      lg_shared_data->data_paths, gen_labeled_klv_name, DATA_FILEPATH_TYPE_KLV);
  char *leaves_filename = data_filepaths_get_writable_filename(
      lg_shared_data->data_paths, gen_labeled_klv_name,
      DATA_FILEPATH_TYPE_LEAVES);

  klv_write(lg_shared_data->klv, gen_labeled_klv_filename);
  klv_write_to_csv(lg_shared_data->klv, lg_shared_data->ld, leaves_filename);

  const int number_of_threads =
      thread_control_get_threads(shared_data->thread_control);

  // Get total game data.
  autoplay_results_combine(lg_shared_data->autoplay_results_list,
                           number_of_threads,
                           lg_shared_data->primary_autoplay_results);

  // Get generational game data
  autoplay_results_reset(lg_shared_data->gen_autoplay_results);
  autoplay_results_combine(lg_shared_data->autoplay_results_list,
                           number_of_threads,
                           lg_shared_data->gen_autoplay_results);

  for (int i = 0; i < number_of_threads; i++) {
    autoplay_results_reset(lg_shared_data->autoplay_results_list[i]);
  }

  // Print info about the current state.
  StringBuilder *leave_gen_sb = string_builder_create();

  string_builder_add_string(
      leave_gen_sb, "************************\n"
                    "Cumulative Autoplay Data\n************************\n\n");

  char *cumul_game_data_str = autoplay_results_to_string(
      lg_shared_data->primary_autoplay_results, true, false);
  string_builder_add_string(leave_gen_sb, cumul_game_data_str);
  free(cumul_game_data_str);

  string_builder_add_string(
      leave_gen_sb,
      "\n**************************\n"
      "Generational Autoplay Data\n**************************\n\n");

  char *gen_game_data_str = autoplay_results_to_string(
      lg_shared_data->gen_autoplay_results, true, false);
  string_builder_add_string(leave_gen_sb, gen_game_data_str);
  free(gen_game_data_str);

  string_builder_add_formatted_string(
      leave_gen_sb,
      "\nAverage Turn Equity: %0.2f\nTarget Minimum "
      "Leave "
      "Count: %d\nLeaves Under "
      "Target Minimum Leave Count: %d\nLeaves Recorded: %d\n\n",
      leave_list_get_empty_leave_mean(lg_shared_data->leave_list),
      leave_list_get_target_leave_count(lg_shared_data->leave_list),
      leave_list_get_leaves_below_target_count(lg_shared_data->leave_list));

  string_builder_add_most_or_least_common_leaves(
      leave_gen_sb, lg_shared_data->leave_list, lg_shared_data->ld, 100, true);
  string_builder_add_string(leave_gen_sb, "\n");
  string_builder_add_most_or_least_common_leaves(
      leave_gen_sb, lg_shared_data->leave_list, lg_shared_data->ld, 100, false);

  char *report_name_prefix =
      cut_off_after_last_char(gen_labeled_klv_filename, '.');
  char *report_name = get_formatted_string("%s_report.txt", report_name_prefix);

  write_string_to_file(report_name, "w", string_builder_peek(leave_gen_sb));
  string_builder_destroy(leave_gen_sb);

  free(report_name);
  free(report_name_prefix);
  free(gen_labeled_klv_filename);
  free(gen_labeled_klv_name);
  free(label);
  free(leaves_filename);

  // Reset data for the next generation.
  leave_list_reset(lg_shared_data->leave_list);
  lg_shared_data->gen_start_games =
      thread_control_get_iter_count(shared_data->thread_control);
  thread_control_increment_max_iter_count(shared_data->thread_control,
                                          shared_data->games_per_gen);
}

typedef struct AutoplayWorker {
  int worker_index;
  const AutoplayArgs *args;
  AutoplayResults *autoplay_results;
  AutoplaySharedData *shared_data;
  XoshiroPRNG *prng;
} AutoplayWorker;

AutoplayWorker *autoplay_worker_create(const AutoplayArgs *args,
                                       const AutoplayResults *target,
                                       int worker_index,
                                       AutoplaySharedData *shared_data) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->args = args;
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->autoplay_results =
      autoplay_results_create_empty_copy(target);
  autoplay_worker->prng = NULL;
  if (shared_data->leavegen_shared_data) {
    autoplay_worker->prng = prng_create(0);
    thread_control_copy_to_dst_and_jump(args->thread_control,
                                        autoplay_worker->prng);
  }
  autoplay_worker->shared_data = shared_data;
  return autoplay_worker;
}

void autoplay_worker_destroy(AutoplayWorker *autoplay_worker) {
  if (!autoplay_worker) {
    return;
  }
  autoplay_results_destroy(autoplay_worker->autoplay_results);
  prng_destroy(autoplay_worker->prng);
  free(autoplay_worker);
}

LeavegenSharedData *
leavegen_shared_data_create(AutoplayResults *primary_autoplay_results,
                            AutoplayResults **autoplay_results_list,
                            const LetterDistribution *ld,
                            const char *data_paths, KLV *klv,
                            int number_of_threads, int target_leave_count) {
  LeavegenSharedData *shared_data = malloc_or_die(sizeof(LeavegenSharedData));

  shared_data->gens_completed = 0;
  shared_data->gen_start_games = 0;
  shared_data->klv = klv;
  shared_data->gen_autoplay_results =
      autoplay_results_create_empty_copy(primary_autoplay_results);
  shared_data->primary_autoplay_results = primary_autoplay_results;
  shared_data->autoplay_results_list = autoplay_results_list;
  shared_data->ld = ld;
  shared_data->data_paths = data_paths;
  shared_data->leave_list = leave_list_create(ld, klv, target_leave_count);
  shared_data->postgen_checkpoint =
      checkpoint_create(number_of_threads, postgen_prebroadcast_func);
  return shared_data;
}

// Use NULL for the KLV when not running in leave gen mode.
AutoplaySharedData *autoplay_shared_data_create(
    AutoplayResults *primary_autoplay_results,
    AutoplayResults **autoplay_results_list, ThreadControl *thread_control,
    const LetterDistribution *ld, const char *data_paths, KLV *klv,
    int number_of_threads, uint64_t games_per_gen, int target_leave_count) {
  AutoplaySharedData *shared_data = malloc_or_die(sizeof(AutoplaySharedData));
  shared_data->games_per_gen = games_per_gen;
  shared_data->thread_control = thread_control;
  shared_data->leavegen_shared_data = NULL;
  if (klv) {
    shared_data->leavegen_shared_data = leavegen_shared_data_create(
        primary_autoplay_results, autoplay_results_list, ld, data_paths, klv,
        number_of_threads, target_leave_count);
  }
  return shared_data;
}

void leavegen_shared_data_destroy(LeavegenSharedData *lg_shared_data) {
  if (!lg_shared_data) {
    return;
  }
  leave_list_destroy(lg_shared_data->leave_list);
  checkpoint_destroy(lg_shared_data->postgen_checkpoint);
  autoplay_results_destroy(lg_shared_data->gen_autoplay_results);
  free(lg_shared_data);
}

void autoplay_shared_data_destroy(AutoplaySharedData *shared_data) {
  if (!shared_data) {
    return;
  }
  leavegen_shared_data_destroy(shared_data->leavegen_shared_data);
  free(shared_data);
}

typedef struct GameRunner {
  bool force_draw;
  int turn_number;
  Game *game;
  MoveList *move_list;
  Rack *leaves[2];
  Rack *original_rack;
  Rack *rare_leave;
  Rack *bag_and_rare_leave_overlap;
  AutoplaySharedData *shared_data;
} GameRunner;

GameRunner *game_runner_create(AutoplayWorker *autoplay_worker) {
  const AutoplayArgs *args = autoplay_worker->args;
  GameRunner *game_runner = malloc_or_die(sizeof(GameRunner));
  const int dist_size = ld_get_size(args->game_args->ld);
  game_runner->leaves[0] = rack_create(dist_size);
  game_runner->leaves[1] = rack_create(dist_size);
  game_runner->original_rack = rack_create(dist_size);
  game_runner->rare_leave = rack_create(dist_size);
  game_runner->bag_and_rare_leave_overlap = rack_create(dist_size);
  game_runner->shared_data = autoplay_worker->shared_data;
  game_runner->game = game_create(args->game_args);
  game_runner->move_list = move_list_create(1);
  return game_runner;
}

void game_runner_destroy(GameRunner *game_runner) {
  if (!game_runner) {
    return;
  }
  rack_destroy(game_runner->leaves[0]);
  rack_destroy(game_runner->leaves[1]);
  rack_destroy(game_runner->original_rack);
  rack_destroy(game_runner->rare_leave);
  rack_destroy(game_runner->bag_and_rare_leave_overlap);
  game_destroy(game_runner->game);
  move_list_destroy(game_runner->move_list);
  free(game_runner);
}

void game_runner_start(AutoplayWorker *autoplay_worker, GameRunner *game_runner,
                       ThreadControlIterOutput *iter_output,
                       int starting_player_index) {
  Game *game = game_runner->game;
  game_reset(game);
  game_seed(game, iter_output->seed);
  game_set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  game_runner->turn_number = 0;
  game_runner->force_draw = false;
  if (game_runner->shared_data->leavegen_shared_data &&
      // We only force draws if we've played enough games for this
      // generation.
      (iter_output->iter_count -
       game_runner->shared_data->leavegen_shared_data->gen_start_games) >=
          (uint64_t)autoplay_worker->args->games_before_force_draw_start) {
    game_runner->force_draw = true;
    for (int i = 0; i < 2; i++) {
      rack_reset(game_runner->leaves[i]);
    }
  }
}

bool game_runner_is_game_over(GameRunner *game_runner) {
  return game_over(game_runner->game) ||
         (game_runner->shared_data->leavegen_shared_data &&
          bag_get_tiles(game_get_bag(game_runner->game)) < (RACK_SIZE));
}

const Move *game_runner_play_move(AutoplayWorker *autoplay_worker,
                                  GameRunner *game_runner) {
  if (game_runner_is_game_over(game_runner)) {
    log_fatal("game runner attempted to play a move when the game is over\n");
  }
  Game *game = game_runner->game;
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  Rack *leave_from_previous_move = game_runner->leaves[player_on_turn_index];
  LeavegenSharedData *lg_shared_data =
      game_runner->shared_data->leavegen_shared_data;
  const int thread_index = autoplay_worker->worker_index;
  // If we are forcing a draw, we need to draw a rare leave. The drawn
  // leave does not necessarily fit in the bag. If we've reached the
  // target minimum leave count for all leaves, no rare leave can be
  // drawn.
  if (game_runner->force_draw &&
      leave_list_get_rare_leave(lg_shared_data->leave_list,
                                autoplay_worker->prng,
                                game_runner->rare_leave)) {
    Rack *player_rack =
        player_get_rack(game_get_player(game, player_on_turn_index));
    const int player_on_turn_draw_index =
        game_get_player_on_turn_draw_index(game);
    // Backup the original rack before returning it to the bag.
    rack_copy(game_runner->original_rack, player_rack);
    return_rack_to_bag(game, player_on_turn_index);

    // Set the rack to the rare leave
    rack_copy(player_rack, game_runner->rare_leave);

    // Remove the rare leave from the bag, for whatever
    // tiles in the rare leave are in the bag. This is to prevent
    // violating the letter distribution. For example, if the rare
    // leave was WWXYYZ, we wouldn't want to randomly draw W, X, Y, or
    // Z from the bag to fill up the rest of the rack since that would violate
    // the letter distribution.
    rack_reset(game_runner->bag_and_rare_leave_overlap);
    draw_leave_from_bag(game_get_bag(game), player_on_turn_draw_index,
                        game_runner->bag_and_rare_leave_overlap,
                        game_runner->rare_leave);
    // Fill up the rest of the rack with tiles from the bag
    draw_to_full_rack(game, player_on_turn_index);

    const Move *forced_move =
        get_top_equity_move(game, thread_index, game_runner->move_list);
    leave_list_add_single_subleave(lg_shared_data->leave_list,
                                   game_runner->rare_leave,
                                   move_get_equity(forced_move));
    // Remove the rare leave from the player's rack and return the
    // tiles that remain since we know for sure that these tiles
    // were drawn from the bag.
    rack_subtract(player_rack, game_runner->rare_leave);
    return_rack_to_bag(game, player_on_turn_index);

    // Add the overlap between the rare leave and the bag back to the
    // player's rack. These were tiles that were in the rare leave but
    // also were in the bag, and thus they were drawn from the bag
    // as described above. These tiles need to be returned to the bag
    // as well.
    rack_copy(player_rack, game_runner->bag_and_rare_leave_overlap);
    return_rack_to_bag(game, player_on_turn_index);

    // Redrawn the original rack.
    draw_rack_from_bag(game, player_on_turn_index, game_runner->original_rack);
  }
  const Move *move =
      get_top_equity_move(game, thread_index, game_runner->move_list);

  if (lg_shared_data) {
    leave_list_add_all_subleaves(
        lg_shared_data->leave_list,
        player_get_rack(game_get_player(game, player_on_turn_index)),
        // Use the bag_and_rare_leave_overlap as a scratch subleave rack
        // for the leave list add all subleaves call. It gets reset
        // before use.
        game_runner->bag_and_rare_leave_overlap, move_get_equity(move));
  }
  play_move(move, game, NULL, leave_from_previous_move);
  game_runner->turn_number++;
  return move;
}

void print_current_status(
    AutoplayWorker *autoplay_worker,
    ThreadControlIterCompletedOutput *iter_completed_output) {
  StringBuilder *status_sb = string_builder_create();
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  string_builder_add_formatted_string(
      status_sb, "Played games %ld in %.3f seconds.",
      iter_completed_output->iter_count_completed,
      iter_completed_output->time_elapsed);
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  if (lg_shared_data) {
    string_builder_add_formatted_string(
        status_sb,
        " Played %ld games in generation %d with %ld leaves under target "
        "count.\n",
        iter_completed_output->iter_count_completed -
            lg_shared_data->gen_start_games,
        lg_shared_data->gens_completed + 1,
        leave_list_get_leaves_below_target_count(lg_shared_data->leave_list));
  } else {
    string_builder_add_string(status_sb, "\n");
  }
  thread_control_print(autoplay_worker->args->thread_control,
                       string_builder_peek(status_sb));
  string_builder_destroy(status_sb);
}

void autoplay_add_game(AutoplayWorker *autoplay_worker, const Game *game,
                       int turns, bool divergent) {
  autoplay_results_add_game(autoplay_worker->autoplay_results, game, turns,
                            divergent);
  ThreadControlIterCompletedOutput iter_completed_output;
  thread_control_complete_iter(autoplay_worker->args->thread_control,
                               &iter_completed_output);
  if (iter_completed_output.print_info) {
    print_current_status(autoplay_worker, &iter_completed_output);
  }
}

void play_autoplay_game_or_game_pair(AutoplayWorker *autoplay_worker,
                                     GameRunner *game_runner1,
                                     GameRunner *game_runner2,
                                     ThreadControlIterOutput *iter_output) {
  const int starting_player_index = iter_output->iter_count % 2;
  game_runner_start(autoplay_worker, game_runner1, iter_output,
                    starting_player_index);
  if (game_runner2) {
    game_runner_start(autoplay_worker, game_runner2, iter_output,
                      1 - starting_player_index);
  }
  bool games_are_divergent = false;
  while (true) {
    const Move *move1 = NULL;
    bool game1_is_over = game_runner_is_game_over(game_runner1);
    if (!game1_is_over) {
      move1 = game_runner_play_move(autoplay_worker, game_runner1);
      autoplay_results_add_move(autoplay_worker->autoplay_results, move1);
    }

    const Move *move2 = NULL;
    bool game2_is_over = true;
    if (game_runner2) {
      game2_is_over = game_runner_is_game_over(game_runner2);
      if (!game2_is_over) {
        move2 = game_runner_play_move(autoplay_worker, game_runner2);
        autoplay_results_add_move(autoplay_worker->autoplay_results, move2);
      }
    }

    if (game1_is_over && game2_is_over) {
      break;
    }

    // It is guaranteed that at least one move is not null
    // at this point.
    if (!move1 || !move2 || compare_moves(move1, move2, true) != -1) {
      games_are_divergent = true;
    }
  }
  autoplay_add_game(autoplay_worker, game_runner1->game,
                    game_runner1->turn_number, games_are_divergent);
  if (game_runner2) {
    // We do not check for min leave counts here because leave gen
    // does not use game pairs and therefore does not have a second
    // game runner.
    autoplay_add_game(autoplay_worker, game_runner2->game,
                      game_runner2->turn_number, games_are_divergent);
  }
}

bool target_min_leave_count_reached(AutoplayWorker *autoplay_worker) {
  const LeavegenSharedData *leavegen_shared_data =
      autoplay_worker->shared_data->leavegen_shared_data;
  return leavegen_shared_data && leave_list_get_leaves_below_target_count(
                                     leavegen_shared_data->leave_list) == 0;
}

void autoplay_single_generation(AutoplayWorker *autoplay_worker,
                                GameRunner *game_runner1,
                                GameRunner *game_runner2) {
  ThreadControl *thread_control = autoplay_worker->args->thread_control;
  ThreadControlIterOutput iter_output;
  while (
      // Check if autoplay was halted by the user.
      !thread_control_get_is_halted(thread_control) &&
      // Check if the maximum iteration has been reached.
      !thread_control_get_next_iter_output(thread_control, &iter_output) &&
      // Check if the target minimum leave count has been reached.
      // This will never be true for the default autoplay mode.
      !target_min_leave_count_reached(autoplay_worker)) {
    play_autoplay_game_or_game_pair(autoplay_worker, game_runner1, game_runner2,
                                    &iter_output);
  }
}

void autoplay_leave_gen(AutoplayWorker *autoplay_worker,
                        GameRunner *game_runner) {
  const AutoplayArgs *args = autoplay_worker->args;
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  for (int i = 0; i < args->gens; i++) {
    autoplay_single_generation(autoplay_worker, game_runner, NULL);
    checkpoint_wait(shared_data->leavegen_shared_data->postgen_checkpoint,
                    shared_data);
    if (thread_control_get_is_halted(args->thread_control)) {
      break;
    }
  }
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;
  const AutoplayArgs *args = autoplay_worker->args;
  GameRunner *game_runner1 = game_runner_create(autoplay_worker);
  GameRunner *game_runner2 = NULL;
  switch (args->type) {
  case AUTOPLAY_TYPE_DEFAULT:
    if (args->use_game_pairs) {
      game_runner2 = game_runner_create(autoplay_worker);
    }
    autoplay_single_generation(autoplay_worker, game_runner1, game_runner2);
    game_runner_destroy(game_runner2);
    break;
  case AUTOPLAY_TYPE_LEAVE_GEN:
    autoplay_leave_gen(autoplay_worker, game_runner1);
    break;
  }

  game_runner_destroy(game_runner1);
  return NULL;
}

autoplay_status_t autoplay(const AutoplayArgs *args,
                           AutoplayResults *autoplay_results) {
  ThreadControl *thread_control = args->thread_control;

  thread_control_reset(thread_control, args->games_per_gen);

  autoplay_results_reset(autoplay_results);

  const int number_of_threads = thread_control_get_threads(thread_control);
  const bool is_leavegen_mode = args->type == AUTOPLAY_TYPE_LEAVE_GEN;

  KLV *klv = NULL;
  bool show_divergent_results = args->use_game_pairs;
  if (is_leavegen_mode) {
    // We can use player index 0 here since it is guaranteed that
    // players share the the KLV.
    klv = players_data_get_klv(args->game_args->players_data, 0);
    show_divergent_results = false;
  }

  AutoplayResults **autoplay_results_list =
      malloc_or_die((sizeof(AutoplayResults *)) * (number_of_threads));

  AutoplaySharedData *shared_data = autoplay_shared_data_create(
      autoplay_results, autoplay_results_list, thread_control,
      args->game_args->ld, args->data_paths, klv, number_of_threads,
      args->games_per_gen, args->target_leave_count);

  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    autoplay_workers[thread_index] = autoplay_worker_create(
        args, autoplay_results, thread_index, shared_data);
    autoplay_results_list[thread_index] =
        autoplay_workers[thread_index]->autoplay_results;
    pthread_create(&worker_ids[thread_index], NULL, autoplay_worker,
                   autoplay_workers[thread_index]);
  }

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
  }

  // If autoplay was interrupted by the user,
  // this will not change the status.
  thread_control_halt(thread_control, HALT_STATUS_MAX_ITERATIONS);

  // The stats have already been combined in leavegen mode
  if (!is_leavegen_mode) {
    autoplay_results_combine(autoplay_results_list, number_of_threads,
                             autoplay_results);
  }

  free(autoplay_results_list);

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    autoplay_worker_destroy(autoplay_workers[thread_index]);
  }

  free(autoplay_workers);
  free(worker_ids);
  autoplay_shared_data_destroy(shared_data);

  players_data_reload(args->game_args->players_data, PLAYERS_DATA_TYPE_KLV,
                      args->data_paths);

  char *autoplay_results_string = autoplay_results_to_string(
      autoplay_results, args->human_readable, show_divergent_results);
  thread_control_print(thread_control, autoplay_results_string);
  free(autoplay_results_string);
  gen_destroy_cache();

  return AUTOPLAY_STATUS_SUCCESS;
}
