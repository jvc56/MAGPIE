
#include "autoplay.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/autoplay_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/bag.h"
#include "../ent/checkpoint.h"
#include "../ent/data_filepaths.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../ent/xoshiro.h"
#include "../str/game_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include "rack_list.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct LeavegenSharedData {
  int num_gens;
  int gens_completed;
  uint64_t gen_start_games;
  int *min_rack_targets;
  AutoplayResults *gen_autoplay_results;
  const LetterDistribution *ld;
  const char *data_paths;
  KLV *klv;
  RackList *rack_list;
  Checkpoint *postgen_checkpoint;
  AutoplayResults *primary_autoplay_results;
  AutoplayResults **autoplay_results_list;
} LeavegenSharedData;

typedef struct AutoplaySharedData {
  int num_threads;
  int print_interval;
  Timer timer;
  uint64_t max_iter_count;
  uint64_t seed;
  XoshiroPRNG *prng;
  uint64_t iter_count;
  cpthread_mutex_t iter_mutex;
  uint64_t iter_count_completed;
  cpthread_mutex_t iter_completed_mutex;
  ThreadControl *thread_control;
  LeavegenSharedData *leavegen_shared_data;
} AutoplaySharedData;

typedef struct AutoplayIterOutput {
  uint64_t seed;
  uint64_t iter_count;
} AutoplayIterOutput;

typedef struct AutoplayIterCompletedOutput {
  uint64_t iter_count_completed;
  double time_elapsed;
  bool print_info;
} AutoplayIterCompletedOutput;

// Returns true if the iter_count is already greater than or equal to
// stop_iter_count and does nothing else.
// Returns false if the iter_count is less than the stop_iter_count
// and increments the iter_count and sets the next seed.
bool autoplay_get_next_iter_output(AutoplaySharedData *shared_data,
                                   AutoplayIterOutput *iter_output) {
  bool at_stop_count = false;
  cpthread_mutex_lock(&shared_data->iter_mutex);
  if (shared_data->iter_count >= shared_data->max_iter_count) {
    at_stop_count = true;
  } else {
    iter_output->seed = prng_next(shared_data->prng);
    iter_output->iter_count = shared_data->iter_count++;
  }
  cpthread_mutex_unlock(&shared_data->iter_mutex);
  return at_stop_count;
}

// This function should be called when a thread has completed computation
// for an iteration given by autoplay_get_next_iter_output.
// It increments the count completed and records the elapsed time.
void autoplay_complete_iter(
    AutoplaySharedData *shared_data,
    AutoplayIterCompletedOutput *iter_completed_output) {
  cpthread_mutex_lock(&shared_data->iter_completed_mutex);
  // Update internal fields
  shared_data->iter_count_completed++;
  // Set output
  iter_completed_output->iter_count_completed =
      shared_data->iter_count_completed;
  iter_completed_output->time_elapsed =
      ctimer_elapsed_seconds(&shared_data->timer);
  iter_completed_output->print_info =
      shared_data->print_interval > 0 &&
      shared_data->iter_count_completed % shared_data->print_interval == 0;
  cpthread_mutex_unlock(&shared_data->iter_completed_mutex);
}

// Copies the thread control PRNG to the other PRNG and performs a PRNG
// jump on the thread control PRNG.
void autoplay_shared_data_copy_to_dst_and_jump(AutoplaySharedData *shared_data,
                                               XoshiroPRNG *dst) {
  prng_copy(dst, shared_data->prng);
  prng_jump(shared_data->prng);
}

void postgen_prebroadcast_func(void *data) {
  AutoplaySharedData *shared_data = (AutoplaySharedData *)data;
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  rack_list_write_to_klv(lg_shared_data->rack_list, lg_shared_data->ld,
                         lg_shared_data->klv);
  lg_shared_data->gens_completed++;

  // Write the KLV for the current generation.
  char *label = get_formatted_string("_gen_%d", lg_shared_data->gens_completed);
  char *gen_labeled_klv_name =
      insert_before_dot(lg_shared_data->klv->name, label);

  ErrorStack *error_stack = error_stack_create();

  char *gen_labeled_klv_filename = data_filepaths_get_writable_filename(
      lg_shared_data->data_paths, gen_labeled_klv_name, DATA_FILEPATH_TYPE_KLV,
      error_stack);
  char *leaves_filename = data_filepaths_get_writable_filename(
      lg_shared_data->data_paths, gen_labeled_klv_name,
      DATA_FILEPATH_TYPE_LEAVES, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("leavegen failed to write results to file");
  }

  klv_write(lg_shared_data->klv, lg_shared_data->data_paths,
            gen_labeled_klv_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("leavegen failed to write klv to file: %s",
              gen_labeled_klv_filename);
  }

  klv_write_to_csv(lg_shared_data->klv, lg_shared_data->ld,
                   lg_shared_data->data_paths, gen_labeled_klv_name, NULL,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("leavegen failed to write klv to CSV");
  }

  // Get total game data.
  autoplay_results_finalize(lg_shared_data->autoplay_results_list,
                            shared_data->num_threads,
                            lg_shared_data->primary_autoplay_results);

  // Get generational game data
  autoplay_results_reset(lg_shared_data->gen_autoplay_results);
  autoplay_results_finalize(lg_shared_data->autoplay_results_list,
                            shared_data->num_threads,
                            lg_shared_data->gen_autoplay_results);

  for (int i = 0; i < shared_data->num_threads; i++) {
    autoplay_results_reset(lg_shared_data->autoplay_results_list[i]);
  }

  // Print info about the current state.
  StringBuilder *leave_gen_sb = string_builder_create();

  string_builder_add_string(
      leave_gen_sb, "************************\n"
                    "Cumulative Autoplay Data\n************************\n\n");

  string_builder_add_formatted_string(
      leave_gen_sb, "Seconds: %f\n",
      ctimer_elapsed_seconds(&shared_data->timer));
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
      "\nTarget Minimum "
      "Leave "
      "Count: %d\nLeaves Under "
      "Target Minimum Leave Count: %d\n\n",
      rack_list_get_target_rack_count(lg_shared_data->rack_list),
      rack_list_get_racks_below_target_count(lg_shared_data->rack_list));

  char *report_name_prefix =
      cut_off_after_last_char(gen_labeled_klv_filename, '.');
  char *report_name = get_formatted_string("%s_report.txt", report_name_prefix);

  write_string_to_file(report_name, "w", string_builder_peek(leave_gen_sb),
                       error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("leavegen failed to write result summary to file");
  }

  string_builder_destroy(leave_gen_sb);
  error_stack_destroy(error_stack);

  free(report_name);
  free(report_name_prefix);
  free(gen_labeled_klv_filename);
  free(gen_labeled_klv_name);
  free(label);
  free(leaves_filename);

  // Reset data for the next generation.
  if (lg_shared_data->gens_completed < lg_shared_data->num_gens) {
    rack_list_reset(
        lg_shared_data->rack_list,
        lg_shared_data->min_rack_targets[lg_shared_data->gens_completed]);
    lg_shared_data->gen_start_games = shared_data->iter_count;
  }
}

typedef struct AutoplayWorker {
  int worker_index;
  const AutoplayArgs *args;
  AutoplayResults *autoplay_results;
  AutoplaySharedData *shared_data;
  XoshiroPRNG *prng;
  int *min_rack_targets;
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
    autoplay_shared_data_copy_to_dst_and_jump(shared_data,
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

LeavegenSharedData *leavegen_shared_data_create(
    AutoplayResults *primary_autoplay_results,
    AutoplayResults **autoplay_results_list, const LetterDistribution *ld,
    const char *data_paths, KLV *klv, int number_of_threads, int num_gens,
    int *min_rack_targets) {
  LeavegenSharedData *shared_data = malloc_or_die(sizeof(LeavegenSharedData));

  shared_data->num_gens = num_gens;
  shared_data->gens_completed = 0;
  shared_data->gen_start_games = 0;
  shared_data->klv = klv;
  shared_data->gen_autoplay_results =
      autoplay_results_create_empty_copy(primary_autoplay_results);
  shared_data->primary_autoplay_results = primary_autoplay_results;
  shared_data->autoplay_results_list = autoplay_results_list;
  shared_data->ld = ld;
  shared_data->data_paths = data_paths;
  shared_data->min_rack_targets = min_rack_targets;
  shared_data->rack_list = rack_list_create(ld, min_rack_targets[0]);
  shared_data->postgen_checkpoint =
      checkpoint_create(number_of_threads, postgen_prebroadcast_func);
  return shared_data;
}

// Use NULL for the KLV when not running in leave gen mode.
AutoplaySharedData *
autoplay_shared_data_create(const AutoplayArgs *args,
                            const uint64_t first_gen_num_games,
                            AutoplayResults *primary_autoplay_results,
                            AutoplayResults **autoplay_results_list, KLV *klv,
                            int num_gens, int *min_rack_targets) {
  AutoplaySharedData *shared_data = malloc_or_die(sizeof(AutoplaySharedData));
  shared_data->num_threads = args->num_threads;
  shared_data->print_interval = args->print_interval;
  ctimer_start(&shared_data->timer);
  shared_data->max_iter_count = first_gen_num_games;
  shared_data->seed = args->seed;
  shared_data->prng = prng_create(args->seed);
  shared_data->iter_count = 0;
  cpthread_mutex_init(&shared_data->iter_mutex);
  shared_data->iter_count_completed = 0;
  cpthread_mutex_init(&shared_data->iter_completed_mutex);
  shared_data->thread_control = args->thread_control;
  shared_data->leavegen_shared_data = NULL;
  if (klv) {
    shared_data->leavegen_shared_data = leavegen_shared_data_create(
        primary_autoplay_results, autoplay_results_list, args->game_args->ld,
        args->data_paths, klv, args->num_threads, num_gens, min_rack_targets);
  }
  return shared_data;
}

void leavegen_shared_data_destroy(LeavegenSharedData *lg_shared_data) {
  if (!lg_shared_data) {
    return;
  }
  rack_list_destroy(lg_shared_data->rack_list);
  checkpoint_destroy(lg_shared_data->postgen_checkpoint);
  autoplay_results_destroy(lg_shared_data->gen_autoplay_results);
  free(lg_shared_data);
}

void autoplay_shared_data_destroy(AutoplaySharedData *shared_data) {
  if (!shared_data) {
    return;
  }
  prng_destroy(shared_data->prng);
  leavegen_shared_data_destroy(shared_data->leavegen_shared_data);
  free(shared_data);
}

typedef struct GameRunner {
  bool force_draw;
  int turn_number;
  int pair_game_number; // 0 for non-paired games, 1 or 2 for game pairs
  uint64_t game_number;
  uint64_t seed;
  Game *game;
  MoveList *move_list;
  AutoplaySharedData *shared_data;
} GameRunner;

GameRunner *game_runner_create(AutoplayWorker *autoplay_worker) {
  const AutoplayArgs *args = autoplay_worker->args;
  GameRunner *game_runner = malloc_or_die(sizeof(GameRunner));
  game_runner->shared_data = autoplay_worker->shared_data;
  game_runner->game = game_create(args->game_args);
  game_runner->move_list = move_list_create(1);
  game_runner->pair_game_number =
      0; // Will be set in game_runner_start if using pairs
  return game_runner;
}

void game_runner_destroy(GameRunner *game_runner) {
  if (!game_runner) {
    return;
  }
  game_destroy(game_runner->game);
  move_list_destroy(game_runner->move_list);
  free(game_runner);
}

void game_runner_start(const AutoplayWorker *autoplay_worker,
                       GameRunner *game_runner,
                       const AutoplayIterOutput *iter_output,
                       int starting_player_index, int pair_game_number) {
  Game *game = game_runner->game;
  game_reset(game);
  game_runner->seed = iter_output->seed;
  game_runner->game_number = iter_output->iter_count;
  game_runner->pair_game_number = pair_game_number;
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
  }
}

bool game_runner_is_game_over(GameRunner *game_runner) {
  return game_over(game_runner->game) ||
         (game_runner->shared_data->leavegen_shared_data &&
          bag_get_letters(game_get_bag(game_runner->game)) < (RACK_SIZE));
}

void game_runner_play_move(AutoplayWorker *autoplay_worker,
                           GameRunner *game_runner, Move **move) {
  if (game_runner_is_game_over(game_runner)) {
    log_fatal("game runner attempted to play a move when the game is over");
  }
  Game *game = game_runner->game;
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  LeavegenSharedData *lg_shared_data =
      game_runner->shared_data->leavegen_shared_data;
  const int thread_index = autoplay_worker->worker_index;
  // If we are forcing a draw, we need to draw a rare leave. The drawn
  // leave does not necessarily fit in the bag. If we've reached the
  // target minimum leave count for all leaves, no rare leave can be
  // drawn.
  Rack *player_rack =
      player_get_rack(game_get_player(game, player_on_turn_index));
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack rare_rack_or_move_leave;
  rack_set_dist_size(&rare_rack_or_move_leave, ld_size);

  if (game_runner->force_draw &&
      rack_list_get_rare_rack(lg_shared_data->rack_list, autoplay_worker->prng,
                              &rare_rack_or_move_leave)) {
    // Backup the original rack
    Rack original_rack;
    rack_copy(&original_rack, player_rack);

    // Set the rack to the rare leave
    rack_copy(player_rack, &rare_rack_or_move_leave);

    const Move *forced_move =
        get_top_equity_move(game, thread_index, game_runner->move_list);
    rack_list_add_rack(lg_shared_data->rack_list, &rare_rack_or_move_leave,
                       equity_to_double(move_get_equity(forced_move)));

    rack_copy(player_rack, &original_rack);
  }
  *move = get_top_equity_move(game, thread_index, game_runner->move_list);

  if (lg_shared_data) {
    rack_list_add_rack(lg_shared_data->rack_list, player_rack,
                       equity_to_double(move_get_equity(*move)));
  }
  get_leave_for_move(*move, game, &rare_rack_or_move_leave);
  autoplay_results_add_move(autoplay_worker->autoplay_results,
                            game_runner->game, *move, &rare_rack_or_move_leave);

  // Print board with move about to be played if requested
  if (autoplay_worker->args->print_boards) {
    StringBuilder *output = string_builder_create();
    if (game_runner->pair_game_number == 0) {
      string_builder_add_formatted_string(
          output, "\n=== Game %llu, Turn %d ===\n",
          (unsigned long long)game_runner->game_number + 1,
          game_runner->turn_number + 1);
    } else {
      string_builder_add_formatted_string(
          output, "\n=== Game Pair %llu, Game %d, Turn %d ===\n",
          (unsigned long long)game_runner->game_number + 1,
          game_runner->pair_game_number, game_runner->turn_number + 1);
    }
    string_builder_add_game(game, game_runner->move_list,
                            autoplay_worker->args->game_string_options, NULL,
                            output);
    string_builder_add_string(output, "\n");
    thread_control_print(autoplay_worker->args->thread_control,
                         string_builder_peek(output));
    string_builder_destroy(output);
  }

  play_move(*move, game, NULL);
  game_runner->turn_number++;
}

void print_current_status(AutoplayWorker *autoplay_worker,
                          AutoplayIterCompletedOutput *iter_completed_output) {
  StringBuilder *status_sb = string_builder_create();
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  string_builder_add_formatted_string(
      status_sb, "Played %ld games in %.3f seconds.",
      iter_completed_output->iter_count_completed,
      iter_completed_output->time_elapsed);
  const LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  if (lg_shared_data) {
    string_builder_add_formatted_string(
        status_sb,
        " Played %ld games in generation %d with %ld rack under target "
        "count.\n",
        iter_completed_output->iter_count_completed -
            lg_shared_data->gen_start_games,
        lg_shared_data->gens_completed + 1,
        rack_list_get_racks_below_target_count(lg_shared_data->rack_list));
  } else {
    string_builder_add_string(status_sb, "\n");
  }
  thread_control_print(autoplay_worker->args->thread_control,
                       string_builder_peek(status_sb));
  string_builder_destroy(status_sb);
}

void autoplay_add_game(AutoplayWorker *autoplay_worker,
                       const GameRunner *game_runner, bool divergent) {
  autoplay_results_add_game(autoplay_worker->autoplay_results,
                            game_runner->game, game_runner->turn_number,
                            divergent, game_runner->seed);
  AutoplayIterCompletedOutput iter_completed_output;
  autoplay_complete_iter(autoplay_worker->shared_data, &iter_completed_output);
  if (iter_completed_output.print_info) {
    print_current_status(autoplay_worker, &iter_completed_output);
  }
}

void play_autoplay_game_or_game_pair(AutoplayWorker *autoplay_worker,
                                     GameRunner *game_runner1,
                                     GameRunner *game_runner2,
                                     const AutoplayIterOutput *iter_output) {
  const int starting_player_index = (int)(iter_output->iter_count % 2);
  game_runner_start(autoplay_worker, game_runner1, iter_output,
                    starting_player_index, game_runner2 ? 1 : 0);
  if (game_runner2) {
    game_runner_start(autoplay_worker, game_runner2, iter_output,
                      1 - starting_player_index, 2);
  }
  bool games_are_divergent = false;
  while (true) {
    Move *move1 = NULL;
    bool game1_is_over = game_runner_is_game_over(game_runner1);
    if (!game1_is_over) {
      game_runner_play_move(autoplay_worker, game_runner1, &move1);
    }

    Move *move2 = NULL;
    bool game2_is_over = true;
    if (game_runner2) {
      game2_is_over = game_runner_is_game_over(game_runner2);
      if (!game2_is_over) {
        game_runner_play_move(autoplay_worker, game_runner2, &move2);
      }
    }

    if (game1_is_over && game2_is_over) {
      break;
    }

    // It is guaranteed that at least one move is not null
    // at this point.
    if (!games_are_divergent &&
        (!move1 || !move2 ||
         compare_moves_without_equity(move1, move2, true) != -1)) {
      games_are_divergent = true;
    }
  }
  autoplay_add_game(autoplay_worker, game_runner1, games_are_divergent);
  if (game_runner2) {
    // We do not check for min leave counts here because leave gen
    // does not use game pairs and therefore does not have a second
    // game runner.
    autoplay_add_game(autoplay_worker, game_runner2, games_are_divergent);
  }
}

bool target_min_leave_count_reached(AutoplayWorker *autoplay_worker) {
  const LeavegenSharedData *leavegen_shared_data =
      autoplay_worker->shared_data->leavegen_shared_data;
  return leavegen_shared_data && rack_list_get_racks_below_target_count(
                                     leavegen_shared_data->rack_list) == 0;
}

void autoplay_single_generation(AutoplayWorker *autoplay_worker,
                                GameRunner *game_runner1,
                                GameRunner *game_runner2) {
  ThreadControl *thread_control = autoplay_worker->args->thread_control;
  AutoplayIterOutput iter_output;
  while (
      // Check if autoplay was exited by the user.
      thread_control_get_status(thread_control) !=
          THREAD_CONTROL_STATUS_USER_INTERRUPT &&
      // Check if the maximum iteration has been reached.
      !autoplay_get_next_iter_output(autoplay_worker->shared_data,
                                     &iter_output) &&
      // Check if the target minimum leave count has been reached.
      // This will never be true for the default autoplay mode.
      !target_min_leave_count_reached(autoplay_worker)) {
    play_autoplay_game_or_game_pair(autoplay_worker, game_runner1, game_runner2,
                                    &iter_output);
  }
}

void autoplay_leave_gen(AutoplayWorker *autoplay_worker,
                        GameRunner *game_runner) {
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  for (int i = 0; i < lg_shared_data->num_gens; i++) {
    autoplay_single_generation(autoplay_worker, game_runner, NULL);
    checkpoint_wait(lg_shared_data->postgen_checkpoint, shared_data);
    if (thread_control_get_status(shared_data->thread_control) ==
        THREAD_CONTROL_STATUS_USER_INTERRUPT) {
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

void parse_min_rack_targets(const AutoplayArgs *args,
                            const StringSplitter *split_min_rack_targets,
                            int *min_rack_targets, ErrorStack *error_stack) {
  int num_gens = string_splitter_get_number_of_items(split_min_rack_targets);
  for (int i = 0; i < num_gens; i++) {
    const char *item = string_splitter_get_item(split_min_rack_targets, i);
    if (is_string_empty_or_whitespace(item)) {
      error_stack_push(
          error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS,
          get_formatted_string("found an empty value for one or more of the "
                               "minimum rack targets: %s",
                               args->num_games_or_min_rack_targets));
      return;
    }
    min_rack_targets[i] = string_to_int(item, error_stack);
    if (!error_stack_is_empty(error_stack) || min_rack_targets[i] < 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS,
          get_formatted_string("failed to parse minimum rack targets: %s",
                               args->num_games_or_min_rack_targets));
      return;
    }
  }
}

void valid_autoplay_results_options(const AutoplayResults *autoplay_results,
                                    const AutoplayArgs *args,
                                    ErrorStack *error_stack) {
  const uint64_t options = autoplay_results_get_options(autoplay_results);
  if (options == 0) {
    return;
  }
  if (options != autoplay_results_build_option(AUTOPLAY_RECORDER_TYPE_GAME) &&
      args->use_game_pairs) {
    error_stack_push(
        error_stack, ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS,
        string_duplicate(
            "the game pairs setting can only be used with the games recorder"));
    return;
  }
}

void autoplay(const AutoplayArgs *args, AutoplayResults *autoplay_results,
              ErrorStack *error_stack) {
  const bool is_leavegen_mode = args->type == AUTOPLAY_TYPE_LEAVE_GEN;
  int num_gens = 1;
  int *min_rack_targets = NULL;
  uint64_t first_gen_num_games;
  if (is_leavegen_mode) {
    StringSplitter *split_min_rack_targets =
        split_string(args->num_games_or_min_rack_targets, ',', false);
    num_gens = string_splitter_get_number_of_items(split_min_rack_targets);
    min_rack_targets = malloc_or_die((sizeof(int)) * (num_gens));
    parse_min_rack_targets(args, split_min_rack_targets, min_rack_targets,
                           error_stack);
    string_splitter_destroy(split_min_rack_targets);
    if (!error_stack_is_empty(error_stack)) {
      free(min_rack_targets);
      error_stack_push(
          error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS,
          get_formatted_string("failed to parse minimum rack targets: %s",
                               args->num_games_or_min_rack_targets));
      return;
    }
    first_gen_num_games = UINT64_MAX;
  } else {
    first_gen_num_games =
        string_to_uint64(args->num_games_or_min_rack_targets, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_AUTOPLAY_MALFORMED_NUM_GAMES,
                       get_formatted_string(
                           "failed to parse the specified number of games: %s",
                           args->num_games_or_min_rack_targets));
      return;
    }
  }

  valid_autoplay_results_options(autoplay_results, args, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  ThreadControl *thread_control = args->thread_control;

  autoplay_results_reset(autoplay_results);

  KLV *klv = NULL;
  bool show_divergent_results = args->use_game_pairs;
  if (is_leavegen_mode) {
    // We can use player index 0 here since it is guaranteed that
    // players share the the KLV.
    klv = players_data_get_klv(args->game_args->players_data, 0);
    show_divergent_results = false;
  }

  AutoplayResults **autoplay_results_list =
      malloc_or_die((sizeof(AutoplayResults *)) * (args->num_threads));

  AutoplaySharedData *shared_data = autoplay_shared_data_create(
      args, first_gen_num_games, autoplay_results, autoplay_results_list, klv,
      num_gens, min_rack_targets);

  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (args->num_threads));
  cpthread_t *worker_ids =
      malloc_or_die((sizeof(cpthread_t)) * (args->num_threads));

  for (int thread_index = 0; thread_index < args->num_threads; thread_index++) {
    autoplay_workers[thread_index] = autoplay_worker_create(
        args, autoplay_results, thread_index, shared_data);
    autoplay_results_list[thread_index] =
        autoplay_workers[thread_index]->autoplay_results;
    cpthread_create(&worker_ids[thread_index], autoplay_worker,
                    autoplay_workers[thread_index]);
  }

  for (int thread_index = 0; thread_index < args->num_threads; thread_index++) {
    cpthread_join(worker_ids[thread_index]);
  }

  // The stats have already been combined in leavegen mode
  if (!is_leavegen_mode) {
    autoplay_results_finalize(autoplay_results_list, args->num_threads,
                              autoplay_results);
  }

  free(autoplay_results_list);

  for (int thread_index = 0; thread_index < args->num_threads; thread_index++) {
    autoplay_worker_destroy(autoplay_workers[thread_index]);
  }

  free(autoplay_workers);
  free(worker_ids);
  autoplay_shared_data_destroy(shared_data);
  free(min_rack_targets);

  // Only reload KLV if it was modified during leavegen
  if (is_leavegen_mode) {
    players_data_reload(args->game_args->players_data, PLAYERS_DATA_TYPE_KLV,
                        args->data_paths, error_stack);
  }

  char *autoplay_results_string = autoplay_results_to_string(
      autoplay_results, args->human_readable, show_divergent_results);
  thread_control_print(thread_control, autoplay_results_string);
  free(autoplay_results_string);
  gen_destroy_cache();
}
