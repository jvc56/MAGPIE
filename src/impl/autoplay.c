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

typedef struct LeavegenSharedData {
  uint64_t gens_completed;
  uint64_t gen_leaves_recorded;
  uint64_t total_leaves_recorded;
  uint64_t failed_force_draws;
  AutoplayResults *gen_autoplay_results;
  const LetterDistribution *ld;
  const char *data_paths;
  KLV *klv;
  LeaveList *leave_list;
  pthread_mutex_t leave_list_mutex;
  Checkpoint *postgen_checkpoint;
  Stat *success_force_draws;
  uint64_t recordable_turns_over_max_force_draw;
  AutoplayResults *primary_autoplay_results;
  AutoplayResults **autoplay_results_list;
} LeavegenSharedData;

typedef struct AutoplaySharedData {
  uint64_t games_per_gen;
  ThreadControl *thread_control;
  Timer *timer;
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

  klv_write(lg_shared_data->klv, gen_labeled_klv_filename);

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

  lg_shared_data->total_leaves_recorded += lg_shared_data->gen_leaves_recorded;
  string_builder_add_formatted_string(
      leave_gen_sb,
      "\nAverage Turn Equity: %0.2f\nLowest Leave Count: %d\nTarget Minimum "
      "Leave "
      "Count: %d\nLeaves Under "
      "Target Minimum Leave Count: %d\nLeaves Recorded: "
      "%d\nForced draws: %d\nFailed forced draws: %d\n",
      leave_list_get_empty_leave_mean(lg_shared_data->leave_list),
      leave_list_get_lowest_leave_count(lg_shared_data->leave_list),
      leave_list_get_target_min_leave_count(lg_shared_data->leave_list),
      leave_list_get_leaves_under_target_min_count(lg_shared_data->leave_list),
      lg_shared_data->total_leaves_recorded,
      stat_get_num_samples(lg_shared_data->success_force_draws),
      lg_shared_data->failed_force_draws);

  if (stat_get_num_samples(lg_shared_data->success_force_draws) > 0) {
    string_builder_add_formatted_string(
        leave_gen_sb,
        "Forced draw average turn: %0.2f %0.2f\n"
        "Recordable turns over max force draw: %d\n\n",
        stat_get_mean(lg_shared_data->success_force_draws) + 1,
        stat_get_stdev(lg_shared_data->success_force_draws),
        lg_shared_data->recordable_turns_over_max_force_draw);
  } else {
    string_builder_add_string(leave_gen_sb, "\n");
  }

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

  // Reset data for the next generation.
  leave_list_reset(lg_shared_data->leave_list);
  stat_reset(lg_shared_data->success_force_draws);
  lg_shared_data->failed_force_draws = 0;
  lg_shared_data->gen_leaves_recorded = 0;
  lg_shared_data->recordable_turns_over_max_force_draw = 0;
  thread_control_increment_max_iter_count(shared_data->thread_control,
                                          shared_data->games_per_gen);
}

typedef struct AutoplayWorker {
  int worker_index;
  const AutoplayArgs *args;
  Rack *original_rack;
  Rack *rare_leave;
  AutoplayResults *autoplay_results;
  AutoplaySharedData *shared_data;
} AutoplayWorker;

AutoplayWorker *autoplay_worker_create(const AutoplayArgs *args,
                                       const AutoplayResults *target,
                                       int worker_index,
                                       AutoplaySharedData *shared_data) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->args = args;
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->original_rack = NULL;
  autoplay_worker->rare_leave = NULL;
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

LeavegenSharedData *
leavegen_shared_data_create(AutoplayResults *primary_autoplay_results,
                            AutoplayResults **autoplay_results_list,
                            const LetterDistribution *ld,
                            const char *data_paths, KLV *klv,
                            int number_of_threads, int target_min_leave_count) {
  LeavegenSharedData *shared_data = malloc_or_die(sizeof(LeavegenSharedData));

  shared_data->gens_completed = 0;
  shared_data->gen_leaves_recorded = 0;
  shared_data->total_leaves_recorded = 0;
  shared_data->recordable_turns_over_max_force_draw = 0;
  shared_data->failed_force_draws = 0;
  shared_data->klv = klv;
  shared_data->gen_autoplay_results =
      autoplay_results_create_empty_copy(primary_autoplay_results);
  shared_data->primary_autoplay_results = primary_autoplay_results;
  shared_data->autoplay_results_list = autoplay_results_list;
  shared_data->ld = ld;
  shared_data->data_paths = data_paths;
  shared_data->leave_list = leave_list_create(ld, klv, target_min_leave_count);
  shared_data->postgen_checkpoint =
      checkpoint_create(number_of_threads, postgen_prebroadcast_func);
  pthread_mutex_init(&shared_data->leave_list_mutex, NULL);
  shared_data->success_force_draws = stat_create(false);
  return shared_data;
}

// Use NULL for the KLV when not running in leave gen mode.
AutoplaySharedData *autoplay_shared_data_create(
    AutoplayResults *primary_autoplay_results,
    AutoplayResults **autoplay_results_list, ThreadControl *thread_control,
    const LetterDistribution *ld, const char *data_paths, KLV *klv,
    int number_of_threads, uint64_t games_per_gen, int target_min_leave_count) {
  AutoplaySharedData *shared_data = malloc_or_die(sizeof(AutoplaySharedData));
  shared_data->games_per_gen = games_per_gen;
  shared_data->thread_control = thread_control;
  shared_data->timer = mtimer_create();
  mtimer_start(shared_data->timer);
  shared_data->leavegen_shared_data = NULL;
  if (klv) {
    shared_data->leavegen_shared_data = leavegen_shared_data_create(
        primary_autoplay_results, autoplay_results_list, ld, data_paths, klv,
        number_of_threads, target_min_leave_count);
  }
  return shared_data;
}

void leavegen_shared_data_destroy(LeavegenSharedData *lg_shared_data) {
  if (!lg_shared_data) {
    return;
  }
  leave_list_destroy(lg_shared_data->leave_list);
  checkpoint_destroy(lg_shared_data->postgen_checkpoint);
  stat_destroy(lg_shared_data->success_force_draws);
  autoplay_results_destroy(lg_shared_data->gen_autoplay_results);
  free(lg_shared_data);
}

void autoplay_shared_data_destroy(AutoplaySharedData *shared_data) {
  if (!shared_data) {
    return;
  }
  mtimer_destroy(shared_data->timer);
  leavegen_shared_data_destroy(shared_data->leavegen_shared_data);
  free(shared_data);
}

void autoplay_leave_list_draw_rarest_available_leave(
    LeavegenSharedData *lg_shared_data, Game *game, int player_on_turn_index,
    int force_draw_turn, Rack *rare_leave) {
  rack_reset(rare_leave);
  pthread_mutex_lock(&lg_shared_data->leave_list_mutex);
  if (leave_list_draw_rarest_available_leave(
          lg_shared_data->leave_list, game_get_bag(game),
          player_get_rack(game_get_player(game, player_on_turn_index)),
          rare_leave, game_get_player_draw_index(game, player_on_turn_index))) {

    stat_push(lg_shared_data->success_force_draws, (double)force_draw_turn, 1);
  } else {
    lg_shared_data->failed_force_draws++;
  }
  pthread_mutex_unlock(&lg_shared_data->leave_list_mutex);
}

// Returns true if the required minimum leave count was reached.
bool autoplay_leave_list_add_leave(LeavegenSharedData *lg_shared_data,
                                   Game *game, int player_on_turn_index,
                                   double move_equity,
                                   int exceeded_max_force_draw,
                                   int target_min_leave_count) {
  pthread_mutex_lock(&lg_shared_data->leave_list_mutex);
  bool target_min_leave_count_reached =
      leave_list_add_leave(
          lg_shared_data->leave_list,
          player_get_rack(game_get_player(game, player_on_turn_index)),
          move_equity) >= target_min_leave_count;
  lg_shared_data->gen_leaves_recorded++;
  lg_shared_data->recordable_turns_over_max_force_draw +=
      exceeded_max_force_draw;
  pthread_mutex_unlock(&lg_shared_data->leave_list_mutex);
  return target_min_leave_count_reached;
}

// Returns true if the required minimum leave count was reached.
bool autoplay_leave_list_add_subleave(LeavegenSharedData *lg_shared_data,
                                      const Rack *subleave, double move_equity,
                                      int target_min_leave_count) {
  pthread_mutex_lock(&lg_shared_data->leave_list_mutex);
  bool min_leave_count_reached =
      leave_list_add_subleave(lg_shared_data->leave_list, subleave,
                              move_equity) >= target_min_leave_count;
  lg_shared_data->gen_leaves_recorded++;
  pthread_mutex_unlock(&lg_shared_data->leave_list_mutex);
  return min_leave_count_reached;
}

// Returns true if the required minimum leave count was reached.
bool play_autoplay_game(AutoplayWorker *autoplay_worker, Game *game,
                        MoveList *move_list, Rack *leaves[2],
                        int starting_player_index, uint64_t seed,
                        uint64_t iter_count) {
  AutoplayResults *autoplay_results = autoplay_worker->autoplay_results;
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  const int thread_index = autoplay_worker->worker_index;

  Rack *original_rack = autoplay_worker->original_rack;
  Rack *rare_leave = autoplay_worker->rare_leave;
  const int max_force_draw_turn = autoplay_worker->args->max_force_draw_turn;
  const int target_min_leave_count =
      autoplay_worker->args->target_min_leave_count;

  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  int turn_number = 0;
  int force_draw_turn_number = -1;
  if (lg_shared_data && max_force_draw_turn >= 0 &&
      iter_count >= (uint64_t)autoplay_worker->args->force_draw_start) {
    force_draw_turn_number = prng_get_random_number(
        bag_get_prng(game_get_bag(game)), max_force_draw_turn + 1);
    for (int i = 0; i < 2; i++) {
      rack_reset(leaves[i]);
    }
  }
  bool min_leave_count_reached = false;
  while (!game_over(game)) {
    const int player_on_turn_index = game_get_player_on_turn_index(game);
    Rack *leave_from_previous_move = leaves[player_on_turn_index];
    if (turn_number == force_draw_turn_number) {
      rack_copy(original_rack,
                player_get_rack(game_get_player(game, player_on_turn_index)));
      return_rack_to_bag(game, player_on_turn_index);
      draw_rack_from_bag(game, player_on_turn_index, leave_from_previous_move);
      autoplay_leave_list_draw_rarest_available_leave(
          lg_shared_data, game, player_on_turn_index, turn_number, rare_leave);
      draw_to_full_rack(game, player_on_turn_index);
      const Move *forced_move =
          get_top_equity_move(game, thread_index, move_list);
      if (autoplay_leave_list_add_subleave(lg_shared_data, rare_leave,
                                           move_get_equity(forced_move),
                                           target_min_leave_count)) {
        min_leave_count_reached = true;
      }
      return_rack_to_bag(game, player_on_turn_index);
      draw_rack_from_bag(game, player_on_turn_index, original_rack);
    }
    const Move *move = get_top_equity_move(game, thread_index, move_list);
    autoplay_results_add_move(autoplay_results, move);
    if (lg_shared_data) {
      if (autoplay_leave_list_add_leave(
              lg_shared_data, game, player_on_turn_index, move_get_equity(move),
              turn_number > max_force_draw_turn, target_min_leave_count)) {
        min_leave_count_reached = true;
      }
    }
    play_move(move, game, NULL, leave_from_previous_move);
    turn_number++;
    if (lg_shared_data && bag_get_tiles(game_get_bag(game)) < (RACK_SIZE)) {
      break;
    }
  }
  autoplay_results_add_game(autoplay_results, game, turn_number, false);
  return min_leave_count_reached;
}

void print_current_status(AutoplayWorker *autoplay_worker,
                          uint64_t iter_count) {
  StringBuilder *status_sb = string_builder_create();
  AutoplaySharedData *shared_data = autoplay_worker->shared_data;
  string_builder_add_formatted_string(
      status_sb, "%d seconds, %ld games",
      (int)mtimer_elapsed_seconds(shared_data->timer), iter_count);
  LeavegenSharedData *lg_shared_data = shared_data->leavegen_shared_data;
  if (lg_shared_data) {
    int gen;
    uint64_t leaves_recorded;
    uint64_t forced_draws;
    int lowest_leave_count;
    pthread_mutex_lock(&lg_shared_data->leave_list_mutex);
    gen = lg_shared_data->gens_completed;
    leaves_recorded = lg_shared_data->gen_leaves_recorded;
    forced_draws = stat_get_num_samples(lg_shared_data->success_force_draws);
    lowest_leave_count =
        leave_list_get_lowest_leave_count(lg_shared_data->leave_list);
    pthread_mutex_unlock(&lg_shared_data->leave_list_mutex);
    string_builder_add_formatted_string(status_sb,
                                        ", gen %d, %ld leaves recorded, %d "
                                        "forced draws, %d lowest leave count\n",
                                        gen, leaves_recorded, forced_draws,
                                        lowest_leave_count);
  } else {
    string_builder_add_string(status_sb, "\n");
  }
  thread_control_print(autoplay_worker->args->thread_control,
                       string_builder_peek(status_sb));
  string_builder_destroy(status_sb);
}

void autoplay_single_generation(AutoplayWorker *autoplay_worker, Game *game,
                                MoveList *move_list, Rack *leaves[2]) {
  const AutoplayArgs *args = autoplay_worker->args;
  ThreadControl *thread_control = args->thread_control;

  const bool use_game_pairs =
      args->use_game_pairs && args->type == AUTOPLAY_TYPE_DEFAULT;
  ThreadControlIterOutput iter_output;

  while (true) {
    if (thread_control_get_is_halted(thread_control)) {
      // The autoplay command was halted by the user
      break;
    }
    if (thread_control_get_next_iter_output(thread_control, &iter_output)) {
      // The max iteration was reached
      break;
    }
    int starting_player_index = iter_output.iter_count % 2;
    if (play_autoplay_game(autoplay_worker, game, move_list, leaves,
                           starting_player_index, iter_output.seed,
                           iter_output.iter_count)) {
      // The minimum leave count was reached
      break;
    }
    if (use_game_pairs) {
      play_autoplay_game(autoplay_worker, game, move_list, leaves,
                         1 - starting_player_index, iter_output.seed,
                         iter_output.iter_count);
    }
    if (iter_output.print_info) {
      print_current_status(autoplay_worker, iter_output.iter_count);
    }
  }
}

void autoplay_leave_gen(AutoplayWorker *autoplay_worker, Game *game,
                        MoveList *move_list) {
  const AutoplayArgs *args = autoplay_worker->args;
  ThreadControl *thread_control = args->thread_control;
  const uint64_t gens = args->gens;
  const LetterDistribution *ld = args->game_args->ld;

  Rack *leaves[2] = {rack_create(ld_get_size(ld)),
                     rack_create(ld_get_size(ld))};
  autoplay_worker->original_rack = rack_create(ld_get_size(ld));
  autoplay_worker->rare_leave = rack_create(ld_get_size(ld));

  AutoplaySharedData *shared_data = autoplay_worker->shared_data;

  for (uint64_t i = 0; i < gens; i++) {
    autoplay_single_generation(autoplay_worker, game, move_list, leaves);
    checkpoint_wait(shared_data->leavegen_shared_data->postgen_checkpoint,
                    shared_data);
    if (thread_control_get_is_halted(thread_control)) {
      break;
    }
  }

  rack_destroy(autoplay_worker->original_rack);
  rack_destroy(autoplay_worker->rare_leave);
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
  thread_control_set_max_iter_count(args->thread_control, args->games_per_gen);

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
      args->games_per_gen, args->target_min_leave_count);

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
      autoplay_results, false, show_divergent_results);
  thread_control_print(thread_control, autoplay_results_string);
  free(autoplay_results_string);
  gen_destroy_cache();

  return AUTOPLAY_STATUS_SUCCESS;
}
