#include "peg_net_eval_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/compat/memory_info.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/peg.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "peg1_net.h"
#include "peg2_net.h"
#include "peg_judge.h"
#include "peg_net_util.h"
#include "peg_world.h"
#include "test_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void peg1_play_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1play needs positions prefix, choices CSV, output prefix, "
              "continuation depth");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int depth = atoi(string_splitter_get_item(fields, 4));
  if (depth != 0 && depth != 2) {
    log_fatal("peg1play invalid continuation");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1),
           *static_list = move_list_create(1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  FILE *choices = fopen_or_die(choices_path, "r");
  path = get_formatted_string("%s.games.csv", prefix);
  FILE *output = fopen_exclusive_or_die(path, "w");
  free(path);
  fprintf(output, "root,candidate,mover,actual_tile,initial_spread,final_"
                  "spread,win,turns,continuation_depth\n");
  char *line = NULL, *choice_line = NULL;
  size_t capacity = 0, choice_capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  int pending_root = -1, pending_candidate = -1;
  bool have_pending = false;
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1play invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1play invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1play CGP load failed");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int current_pass;
    const int count =
        peg_world_static_pool(root, pool, candidates, &current_pass);
    if (count != expected || current_pass != pass_rank) {
      log_fatal("peg1play candidate replay mismatch");
    }
    int actual_tile = -1;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
        actual_tile = ml;
      }
    }
    // Consume this root's requested candidates (choices sorted by root).
    while (true) {
      if (!have_pending) {
        if (getline_ignore_carriage_return(&choice_line, &choice_capacity,
                                           choices) < 0) {
          break;
        }
        if (sscanf(choice_line, "%d,%d", &pending_root, &pending_candidate) !=
            2) {
          continue;
        }
        have_pending = true;
      }
      if (pending_root != root_id) {
        if (pending_root < root_id) {
          log_fatal("peg1play choices out of order");
        }
        break;
      }
      have_pending = false;
      if (pending_candidate < 0 || pending_candidate >= count) {
        log_fatal("peg1play invalid candidate");
      }
      game_copy(game, root);
      const double initial = spread_for_player(game, seat);
      play_move(&candidates[pending_candidate], game, NULL);
      if (bag_get_letters(game_get_bag(game)) != 0) {
        log_fatal("peg1play actual draw not consumed");
      }
      int turns = 1;
      while (!game_over(game) && turns < PEG_NET_MAX_TURNS) {
        Move next;
        if (depth == 2) {
          fixed_depth_endgame_move(game, &next, 2);
        } else {
          move_copy(&next, top_equity_move(game, static_list));
        }
        play_move(&next, game, NULL);
        turns++;
      }
      if (!game_over(game)) {
        log_fatal("peg1play continuation did not reach game over");
      }
      const double final = spread_for_player(game, seat);
      fprintf(output, "%d,%d,%d,%d,%.9g,%.9g,%.1f,%d,%d\n", root_id,
              pending_candidate, seat, actual_tile, initial, final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              turns, depth);
    }
    fflush(output);
  }
  if (have_pending) {
    log_fatal("peg1play choice for a root not in positions");
  }
  free(line);
  free(choice_line);
  fclose(positions);
  fclose(choices);
  fclose(output);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  game_destroy(game);
  config_destroy(config);
}

void peg1_utility_net_eval_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 9) {
    log_fatal("peg1util needs first model, second model|-, shortlist, "
              "w_spread, scale, positions prefix, output prefix, check");
  }
  Peg1UtilityNet *first =
      peg1_utility_net_load(string_splitter_get_item(fields, 1));
  const char *second_path = string_splitter_get_item(fields, 2);
  Peg1UtilityNet *second = strings_equal(second_path, "-")
                               ? NULL
                               : peg1_utility_net_load(second_path);
  const int shortlist = atoi(string_splitter_get_item(fields, 3));
  const Peg1UtilityWeights utility = {
      .w_win = 1.0,
      .w_spread = strtod(string_splitter_get_item(fields, 4), NULL),
      .scale = strtod(string_splitter_get_item(fields, 5), NULL)};
  const char *data = string_splitter_get_item(fields, 6);
  const char *prefix = string_splitter_get_item(fields, 7);
  const bool check = atoi(string_splitter_get_item(fields, 8)) != 0;
  if (utility.scale <= 0 || (second != NULL && shortlist < 1)) {
    log_fatal("peg1util invalid scale or shortlist");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *swapped = game_duplicate(root);
  Peg1Scratch work = peg1_scratch_create(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  const char *suffixes[] = {"decisions.csv", "choices.csv", "inputs.f32"};
  FILE *output[3];
  for (int idx = 0; idx < 3; idx++) {
    path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    output[idx] = fopen_exclusive_or_die(path, "wb");
    free(path);
  }
  fprintf(output[0], "root,candidates,scored,choice,win,spread,movegen_ms,"
                     "world_ms,opp_ms,mover_ms,head_ms,stage2_ms,total_ms,"
                     "privacy_checked\n");
  const int width1 = first->inputs;
  const int width2 = second == NULL ? 1 : second->inputs;
  float *inputs = malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE * width1);
  float *inputs2 = malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE * width2);
  float *private_inputs =
      malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE * width1);
  double values[PEG_STATIC_POOL_SIZE], wins[PEG_STATIC_POOL_SIZE],
      spreads[PEG_STATIC_POOL_SIZE];
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first_row, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first_row, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1util invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1util invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1util CGP load failed");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int count;
    Peg1NetTiming timing = {0}, timing2 = {0};
    const int64_t start = ctimer_monotonic_ns();
    const int choice = peg1_utility_net_choose(
        first, second, shortlist, &utility, root, pool, candidates, &count,
        &work, inputs, inputs2, values, wins, spreads, &timing, &timing2);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count != expected) {
      log_fatal("peg1util candidate replay mismatch");
    }
    const int scored = count < first->top_k ? count : first->top_k;
    if (check) {
      int unseen[VALUE_NET_TILE_TYPES];
      peg_world_public_unseen(root, unseen);
      int actual = -1, other = -1;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
          actual = ml;
        }
      }
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
        if (unseen[ml] > 0 && ml != actual) {
          other = ml;
        }
      }
      peg_world_from_public_counts(root, unseen, other < 0 ? actual : other,
                                   swapped);
      Move private_candidates[PEG_STATIC_POOL_SIZE];
      int private_count;
      Peg1NetTiming ignored = {0}, ignored2 = {0};
      double private_values[PEG_STATIC_POOL_SIZE],
          private_wins[PEG_STATIC_POOL_SIZE],
          private_spreads[PEG_STATIC_POOL_SIZE];
      const int private_choice = peg1_utility_net_choose(
          first, second, shortlist, &utility, swapped, pool, private_candidates,
          &private_count, &work, private_inputs, inputs2, private_values,
          private_wins, private_spreads, &ignored, &ignored2);
      if (private_count != count || private_choice != choice ||
          memcmp(inputs, private_inputs, sizeof(float) * scored * width1) !=
              0) {
        log_fatal("1PEG utility chooser depends on the hidden last tile");
      }
    }
    double stage2_ms = timing2.world_ms + timing2.opp_ms + timing2.mover_ms +
                       timing2.scalar_ms + timing2.head_ms;
    fprintf(output[0],
            "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d\n",
            root_id, count, scored, choice, choice >= 0 ? wins[choice] : 0,
            choice >= 0 ? spreads[choice] : 0, timing.movegen_ms,
            timing.world_ms, timing.opp_ms, timing.mover_ms, timing.head_ms,
            stage2_ms, total_ms, check);
    fprintf(output[1], "%d,0\n", root_id);
    if (choice > 0) {
      fprintf(output[1], "%d,%d\n", root_id, choice);
    }
    if (!fwrite_le_floats(inputs, (size_t)scored * width1, output[2])) {
      log_fatal("peg1util write failed");
    }
    for (int idx = 0; idx < 3; idx++) {
      fflush(output[idx]);
    }
  }
  free(line);
  free(inputs);
  free(inputs2);
  free(private_inputs);
  fclose(positions);
  for (int idx = 0; idx < 3; idx++) {
    fclose(output[idx]);
  }
  error_stack_destroy(errors);
  move_list_destroy(pool);
  peg1_scratch_destroy(&work);
  game_destroy(swapped);
  config_destroy(config);
  peg1_utility_net_destroy(first);
  if (second != NULL) {
    peg1_utility_net_destroy(second);
  }
}

void peg1_game_pairs_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 11) {
    log_fatal("peg1games3 needs first, second|-, shortlist, w_spread, scale, "
              "count, offset, seed, depth, prefix");
  }
  Peg1UtilityNet *first =
      peg1_utility_net_load(string_splitter_get_item(fields, 1));
  const char *second_path = string_splitter_get_item(fields, 2);
  Peg1UtilityNet *second = strings_equal(second_path, "-")
                               ? NULL
                               : peg1_utility_net_load(second_path);
  const int shortlist = atoi(string_splitter_get_item(fields, 3));
  const Peg1UtilityWeights utility = {
      .w_win = 1.0,
      .w_spread = strtod(string_splitter_get_item(fields, 4), NULL),
      .scale = strtod(string_splitter_get_item(fields, 5), NULL)};
  const int games = atoi(string_splitter_get_item(fields, 6));
  const int offset = atoi(string_splitter_get_item(fields, 7));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 8), NULL, 10);
  const int depth = atoi(string_splitter_get_item(fields, 9));
  const char *prefix = string_splitter_get_item(fields, 10);
  if (games < 1 || offset < 0 || (depth != 0 && depth != 2)) {
    log_fatal("peg1games3 invalid arguments");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *branch = game_duplicate(game);
  Peg1Scratch work = peg1_scratch_create(game);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.fullgames.csv", prefix);
  FILE *output = fopen_exclusive_or_die(path, "w");
  free(path);
  fprintf(output, "game,source_seed,starter,peg1,mover,net_choice,candidates,"
                  "static_final,net_final,static_win,net_win,decision_ms,"
                  "continuation_depth\n");
  float *inputs =
      malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE * first->inputs);
  float *inputs2 = malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE *
                                 (second == NULL ? 1 : second->inputs));
  double values[PEG_STATIC_POOL_SIZE], wins[PEG_STATIC_POOL_SIZE],
      spreads[PEG_STATIC_POOL_SIZE];
  for (int local = 0; local < games; local++) {
    const int game_id = offset + local;
    const uint64_t source_seed = splitmix64(
        seed ^ splitmix64((uint64_t)game_id) ^ splitmix64(UINT64_C(918273)));
    game_reset(game);
    game_seed(game, source_seed);
    game_set_starting_player_index(game, game_id % 2);
    draw_starting_racks(game);
    int turns = 0;
    while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
           turns < PEG_NET_MAX_TURNS) {
      Move played;
      move_copy(&played, top_equity_move(game, static_list));
      play_move(&played, game, NULL);
      turns++;
    }
    const bool peg1 =
        !game_over(game) && bag_get_letters(game_get_bag(game)) == 1;
    double finals[2] = {0, 0};
    int count = 0, choice = 0, mover = -1;
    double decision_ms = 0;
    if (peg1) {
      mover = game_get_player_on_turn_index(game);
      Move candidates[PEG_STATIC_POOL_SIZE];
      Peg1NetTiming timing = {0}, timing2 = {0};
      const int64_t start = ctimer_monotonic_ns();
      choice = peg1_utility_net_choose(
          first, second, shortlist, &utility, game, pool, candidates, &count,
          &work, inputs, inputs2, values, wins, spreads, &timing, &timing2);
      decision_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
      for (int arm = 0; arm < 2; arm++) {
        game_copy(branch, game);
        Move first_move;
        if (choice < 0) {
          // No placement: both policies must pass.
          move_set_as_pass(&first_move);
        } else {
          move_copy(&first_move, &candidates[arm == 0 ? 0 : choice]);
        }
        play_move(&first_move, branch, NULL);
        int steps = 0;
        while (!game_over(branch) && steps < PEG_NET_MAX_TURNS) {
          Move next;
          if (depth == 2 && bag_get_letters(game_get_bag(branch)) == 0) {
            fixed_depth_endgame_move(branch, &next, 2);
          } else {
            move_copy(&next, top_equity_move(branch, static_list));
          }
          play_move(&next, branch, NULL);
          steps++;
        }
        if (!game_over(branch)) {
          log_fatal("peg1games3 continuation did not end");
        }
        finals[arm] = spread_for_player(branch, mover);
      }
    } else {
      while (!game_over(game) && turns < PEG_NET_MAX_TURNS) {
        Move played;
        move_copy(&played, top_equity_move(game, static_list));
        play_move(&played, game, NULL);
        turns++;
      }
      finals[0] = finals[1] = spread_for_player(game, 0);
    }
    fprintf(output, "%d,%llu,%d,%d,%d,%d,%d,%.9g,%.9g,%.1f,%.1f,%.9g,%d\n",
            game_id, (unsigned long long)source_seed, game_id % 2, peg1, mover,
            choice, count, finals[0], finals[1],
            finals[0] > 0   ? 1.0
            : finals[0] < 0 ? 0.0
                            : .5,
            finals[1] > 0   ? 1.0
            : finals[1] < 0 ? 0.0
                            : .5,
            decision_ms, depth);
    fflush(output);
  }
  free(inputs);
  free(inputs2);
  fclose(output);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  peg1_scratch_destroy(&work);
  game_destroy(branch);
  config_destroy(config);
  peg1_utility_net_destroy(first);
  if (second != NULL) {
    peg1_utility_net_destroy(second);
  }
}

// MAGPIE's own PEG greedy seed (peg_solve stage 0) as a 1PEG chooser, over
// every legal move or only the static top-64 placements. One thread, full
// scenario enumeration, rational opponent, no nested lookahead. The chosen
// move is played with the stored game's actual last tile and finished with
// both common continuations (static, fixed two-ply endgame).
static double peg1_play_out(const Game *root, const Move *move, int seat,
                            int depth, Game *game, MoveList *static_list) {
  game_copy(game, root);
  play_move(move, game, NULL);
  int turns = 1;
  while (!game_over(game) && turns < PEG_NET_MAX_TURNS) {
    Move next;
    if (depth == 2 && bag_get_letters(game_get_bag(game)) == 0) {
      fixed_depth_endgame_move(game, &next, 2);
    } else {
      move_copy(&next, top_equity_move(game, static_list));
    }
    play_move(&next, game, NULL);
    turns++;
  }
  if (!game_over(game)) {
    log_fatal("peg1greedy continuation did not end");
  }
  return spread_for_player(game, seat);
}

static void greedy_seed_pick(const Game *root, const Move *candidates,
                             int count, bool top64, Move *best,
                             double *solve_ms) {
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *errors = error_stack_create();
  const Move *only[PEG_STATIC_POOL_SIZE];
  for (int idx = 0; idx < count; idx++) {
    only[idx] = &candidates[idx];
  }
  const PegArgs args = {.game = root,
                        .thread_control = control,
                        .num_threads = 1,
                        .time_budget_seconds = 0,
                        .greedy_seed_only = true,
                        .opp_model = PEG_OPP_RATIONAL,
                        .scenario_stride = 1,
                        .nested_enabled = false,
                        .only_moves = top64 ? only : NULL,
                        .n_only_moves = top64 ? count : 0};
  PegResult result;
  const int64_t start = ctimer_monotonic_ns();
  peg_solve(&args, &result, errors);
  *solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (!error_stack_is_empty(errors) || result.n_top_cands < 1) {
    error_stack_print_and_reset(errors);
    log_fatal("peg1greedy peg_solve failed");
  }
  move_copy(best, &result.best_move);
  peg_result_destroy(&result);
  error_stack_destroy(errors);
  thread_control_destroy(control);
}

void peg1_greedy_seed_eval_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 4) {
    log_fatal("peg1greedy needs positions prefix, output prefix, check");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const bool check = atoi(string_splitter_get_item(fields, 3)) != 0;
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *swapped = game_duplicate(root), *game = game_duplicate(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.greedy.csv", prefix);
  FILE *output = fopen_exclusive_or_die(path, "w");
  free(path);
  fprintf(output, "root,variant,choice,pass,solve_ms,final_d0,win_d0,"
                  "final_d2,win_d2,privacy_checked,move\n");
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1greedy invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1greedy invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1greedy CGP load failed");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int current_pass;
    const int count =
        peg_world_static_pool(root, pool, candidates, &current_pass);
    if (count != expected) {
      log_fatal("peg1greedy candidate replay mismatch");
    }
    for (int variant = 0; variant < 2; variant++) {
      const bool top64 = variant == 1;
      Move best;
      double solve_ms;
      greedy_seed_pick(root, candidates, count, top64, &best, &solve_ms);
      if (check) {
        int unseen[VALUE_NET_TILE_TYPES];
        peg_world_public_unseen(root, unseen);
        int actual = -1, other = -1;
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
          if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
            actual = ml;
          }
        }
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
          if (unseen[ml] > 0 && ml != actual) {
            other = ml;
          }
        }
        peg_world_from_public_counts(root, unseen, other < 0 ? actual : other,
                                     swapped);
        Move private_candidates[PEG_STATIC_POOL_SIZE];
        int private_pass;
        const int private_count = peg_world_static_pool(
            swapped, pool, private_candidates, &private_pass);
        Move private_best;
        double ignored;
        greedy_seed_pick(swapped, private_candidates, private_count, top64,
                         &private_best, &ignored);
        if (compare_moves(&best, &private_best, true) != -1) {
          log_fatal("peg1greedy choice depends on the hidden last tile");
        }
      }
      int choice = -1;
      for (int idx = 0; idx < count && choice < 0; idx++) {
        if (compare_moves(&best, &candidates[idx], true) == -1) {
          choice = idx;
        }
      }
      const double final0 =
          peg1_play_out(root, &best, seat, 0, game, static_list);
      const double final2 =
          peg1_play_out(root, &best, seat, 2, game, static_list);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &best,
                              game_get_ld(root), false);
      fprintf(output, "%d,%s,%d,%d,%.9g,%.9g,%.1f,%.9g,%.1f,%d,\"%s\"\n",
              root_id, top64 ? "top64" : "all", choice,
              move_get_type(&best) == GAME_EVENT_PASS, solve_ms, final0,
              final0 > 0   ? 1.0
              : final0 < 0 ? 0.0
                           : .5,
              final2,
              final2 > 0   ? 1.0
              : final2 < 0 ? 0.0
                           : .5,
              check, string_builder_peek(name));
    }
    fflush(output);
  }
  free(line);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  game_destroy(swapped);
  game_destroy(game);
  config_destroy(config);
}

enum {
  PEG1_PICK_GREEDY_ALL = -1,
  PEG1_PICK_DEFAULT_PEG = -2,
  PEG1_PICK_DEFAULT_PEG_CHECKED = -3,
};

void peg1_exact_eval_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 6) {
    log_fatal("peg1exact needs positions prefix, choices CSV, output prefix, "
              "plies, tt fraction");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 5), NULL);
  if (plies < 1 || plies > 25 || tt_fraction <= 0 || tt_fraction > 0.1) {
    log_fatal("peg1exact invalid plies or tt fraction");
  }
  // Load the picks and any finished output.
  enum { PEG1_MAX_PICKS = 1 << 18 };
  int *pick_root = malloc_or_die(sizeof(int) * PEG1_MAX_PICKS);
  int *pick_code = malloc_or_die(sizeof(int) * PEG1_MAX_PICKS);
  bool *pick_done = calloc_or_die(PEG1_MAX_PICKS, sizeof(bool));
  int num_picks = 0;
  char *line = NULL;
  size_t capacity = 0;
  FILE *choices = fopen_or_die(choices_path, "r");
  while (getline_ignore_carriage_return(&line, &capacity, choices) >= 0) {
    int root_id, code;
    if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
      if (num_picks == PEG1_MAX_PICKS) {
        log_fatal("peg1exact too many picks");
      }
      pick_root[num_picks] = root_id;
      pick_code[num_picks++] = code;
    }
  }
  fclose(choices);
  char *out_path = get_formatted_string("%s.exact.csv", prefix);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    // A run killed before its first flush leaves an empty file: write the
    // header again.
    fresh = fgetc(previous) == EOF;
    rewind(previous);
  }
  if (previous != NULL) {
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id, code;
      if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
        for (int idx = 0; idx < num_picks; idx++) {
          if (pick_root[idx] == root_id && pick_code[idx] == code) {
            pick_done[idx] = true;
          }
        }
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output, "root,code,choice,pass,final,win,proven,solve_ms,peg_ms,"
                    "peg_turns,pick_ms,move\n");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  PegExactSolver exact = peg_exact_create(plies, tt_fraction);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1exact invalid position");
    }
    bool pending = false;
    for (int idx = 0; idx < num_picks; idx++) {
      pending |= pick_root[idx] == root_id && !pick_done[idx];
    }
    if (!pending) {
      continue;
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1exact invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1exact CGP load failed");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int current_pass;
    const int count =
        peg_world_static_pool(root, pool, candidates, &current_pass);
    if (count != expected) {
      log_fatal("peg1exact candidate replay mismatch");
    }
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || pick_done[idx]) {
        continue;
      }
      const int code = pick_code[idx];
      Move pick;
      double pick_ms = 0;
      if (code >= 0) {
        if (code >= count) {
          log_fatal("peg1exact invalid candidate");
        }
        move_copy(&pick, &candidates[code]);
      } else if (code == PEG1_PICK_GREEDY_ALL) {
        greedy_seed_pick(root, candidates, count, false, &pick, &pick_ms);
      } else if (code == PEG1_PICK_DEFAULT_PEG) {
        default_peg_pick(root, 1, NULL, NULL, &pick, &pick_ms);
      } else if (code == PEG1_PICK_DEFAULT_PEG_CHECKED) {
        // Information rule: the pick must not change when the actual last
        // tile is swapped with a tile from the opponent's rack.
        default_peg_pick(root, 1, NULL, NULL, &pick, &pick_ms);
        int unseen[VALUE_NET_TILE_TYPES];
        peg_world_public_unseen(root, unseen);
        int actual = -1, other = -1;
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
          if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
            actual = ml;
          }
        }
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
          if (unseen[ml] > 0 && ml != actual) {
            other = ml;
          }
        }
        peg_world_from_public_counts(root, unseen, other < 0 ? actual : other,
                                     game);
        Move private_pick;
        double ignored;
        default_peg_pick(game, 1, NULL, NULL, &private_pick, &ignored);
        if (compare_moves(&pick, &private_pick, true) != -1) {
          log_fatal("default PEG pick depends on the hidden last tile");
        }
      } else {
        log_fatal("peg1exact invalid pick code");
      }
      int choice = -1;
      for (int cand = 0; cand < count && choice < 0; cand++) {
        if (compare_moves(&pick, &candidates[cand], true) == -1) {
          choice = cand;
        }
      }
      bool proven;
      double solve_ms, peg_ms;
      int peg_turns;
      const double final =
          peg_exact_final_spread(&exact, root, &pick, seat, game, &proven,
                                 &solve_ms, &peg_ms, &peg_turns);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output, "%d,%d,%d,%d,%.9g,%.1f,%d,%.9g,%.9g,%d,%.9g,\"%s\"\n",
              root_id, code, choice, move_get_type(&pick) == GAME_EVENT_PASS,
              final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              proven, solve_ms, peg_ms, peg_turns, pick_ms,
              string_builder_peek(name));
      pick_done[idx] = true;
    }
    fflush(output);
    fsync(fileno(output));
  }
  free(line);
  fclose(positions);
  fclose(output);
  peg_exact_destroy(&exact);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  game_destroy(game);
  config_destroy(config);
  free(pick_root);
  free(pick_code);
  free(pick_done);
}

// peg2eval pick codes besides the network ranks.
enum {
  PEG2_PICK_DEFAULT_PEG = -2,
  PEG2_PICK_NET_PEG = -4,
  PEG2_PICK_NET_PEG_CHECKED = -5,
  PEG2_PICK_NET_PEG_UNCACHED = -6,
  // Most candidates a PEG2_EVAL_ONLY line may list for one root.
  PEG2_EVAL_ONLY_MAX = 64,
};

void peg1_utility_net_predict_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 6) {
    log_fatal("peg1pred needs fast model, strong model, positions prefix, "
              "output prefix, max rank");
  }
  Peg1UtilityNet *nets[2] = {
      peg1_utility_net_load(string_splitter_get_item(fields, 1)),
      peg1_utility_net_load(string_splitter_get_item(fields, 2))};
  const char *data = string_splitter_get_item(fields, 3);
  const char *prefix = string_splitter_get_item(fields, 4);
  const int max_rank = atoi(string_splitter_get_item(fields, 5));
  if (max_rank < 1 || max_rank > PEG_STATIC_POOL_SIZE) {
    log_fatal("peg1pred invalid max rank");
  }
  const Peg1UtilityWeights utility = {
      .w_win = 1.0, .w_spread = 0.5, .scale = 100};
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Peg1Scratch work = peg1_scratch_create(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.pred.csv", prefix);
  FILE *output = fopen_exclusive_or_die(path, "wb");
  free(path);
  fprintf(output, "root,rank,candidates,tiles,fast_win,fast_spread,strong_win,"
                  "strong_spread,move\n");
  int max_inputs = 1;
  for (int net_idx = 0; net_idx < 2; net_idx++) {
    if (nets[net_idx]->inputs > max_inputs) {
      max_inputs = nets[net_idx]->inputs;
    }
  }
  float *inputs =
      malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE * max_inputs);
  double values[PEG_STATIC_POOL_SIZE], wins[2][PEG_STATIC_POOL_SIZE],
      spreads[2][PEG_STATIC_POOL_SIZE];
  int scored[2];
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 1, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int count = 0;
    for (int net_idx = 0; net_idx < 2; net_idx++) {
      Peg1NetTiming timing = {0}, timing2 = {0};
      (void)peg1_utility_net_choose(nets[net_idx], NULL, 0, &utility, root,
                                    pool, candidates, &count, &work, inputs,
                                    NULL, values, wins[net_idx],
                                    spreads[net_idx], &timing, &timing2);
      scored[net_idx] =
          count < nets[net_idx]->top_k ? count : nets[net_idx]->top_k;
    }
    if (count != expected) {
      log_fatal("peg1pred candidate replay mismatch");
    }
    for (int rank = 0; rank < count && rank < max_rank; rank++) {
      fprintf(output, "%d,%d,%d,%d", root_id, rank, count,
              move_get_tiles_played(&candidates[rank]));
      for (int net_idx = 0; net_idx < 2; net_idx++) {
        if (rank < scored[net_idx]) {
          fprintf(output, ",%.9g,%.9g", wins[net_idx][rank],
                  spreads[net_idx][rank]);
        } else {
          fprintf(output, ",,");
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &candidates[rank],
                              game_get_ld(root), false);
      fprintf(output, ",\"%s\"\n", string_builder_peek(name));
    }
    fflush(output);
  }
  free(line);
  free(inputs);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  peg1_scratch_destroy(&work);
  config_destroy(config);
  for (int net_idx = 0; net_idx < 2; net_idx++) {
    peg1_utility_net_destroy(nets[net_idx]);
  }
}

void peg2_exact_eval_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 12) {
    log_fatal("peg2eval needs positions prefix, choices, output prefix, "
              "reply, plies, tt fraction, first, second|-, shortlist, "
              "w_spread, scale");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const char *reply_name = string_splitter_get_item(fields, 4);
  const int plies = atoi(string_splitter_get_item(fields, 5));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 6), NULL);
  Peg2ReplyPolicy reply;
  if (strings_equal(reply_name, "net")) {
    reply = PEG2_REPLY_NET;
  } else if (strings_equal(reply_name, "peg")) {
    reply = PEG2_REPLY_PEG;
  } else {
    log_fatal("peg2eval reply must be net or peg");
  }
  if (plies < 1 || plies > 25 || tt_fraction <= 0 || tt_fraction > 0.1) {
    log_fatal("peg2eval invalid plies or tt fraction");
  }
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * pick_capacity);
  bool *pick_done = NULL;
  char *line = NULL;
  size_t capacity = 0;
  FILE *choices = fopen_or_die(choices_path, "r");
  while (getline_ignore_carriage_return(&line, &capacity, choices) >= 0) {
    int root_id, code;
    if (sscanf(line, "%d,%d", &root_id, &code) != 2) {
      continue;
    }
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root = realloc_or_die(pick_root, sizeof(int) * pick_capacity);
      pick_code = realloc_or_die(pick_code, sizeof(int) * pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks++] = code;
  }
  fclose(choices);
  // PEG2_EVAL_ONLY: a CSV of "root,rank,rank,..." lines (ranks in the root's
  // static candidate pool, e.g. a network's top K). When set, the PEG pick
  // codes solve only the listed candidates of their root.
  const char *only_path = getenv("PEG2_EVAL_ONLY");
  int only_roots = 0;
  int *only_root = NULL, *only_count = NULL, *only_rank = NULL;
  if (only_path != NULL && only_path[0] != '\0') {
    int only_capacity = 64;
    only_root = malloc_or_die(sizeof(int) * only_capacity);
    only_count = malloc_or_die(sizeof(int) * only_capacity);
    only_rank = malloc_or_die(sizeof(int) * only_capacity * PEG2_EVAL_ONLY_MAX);
    FILE *only_file = fopen_or_die(only_path, "r");
    while (getline_ignore_carriage_return(&line, &capacity, only_file) >= 0) {
      char *cursor = line;
      char *end;
      const long root_id = strtol(cursor, &end, 10);
      if (end == cursor) {
        continue;
      }
      if (only_roots == only_capacity) {
        only_capacity *= 2;
        only_root = realloc_or_die(only_root, sizeof(int) * only_capacity);
        only_count = realloc_or_die(only_count, sizeof(int) * only_capacity);
        only_rank = realloc_or_die(only_rank, sizeof(int) * only_capacity *
                                                  PEG2_EVAL_ONLY_MAX);
      }
      int listed = 0;
      cursor = end;
      while (*cursor == ',') {
        const long rank = strtol(cursor + 1, &end, 10);
        if (end == cursor + 1 || listed == PEG2_EVAL_ONLY_MAX || rank < 0) {
          log_fatal("peg2eval invalid PEG2_EVAL_ONLY line");
        }
        only_rank[only_roots * PEG2_EVAL_ONLY_MAX + listed++] = (int)rank;
        cursor = end;
      }
      if (listed < 1) {
        log_fatal("peg2eval PEG2_EVAL_ONLY line lists no candidates");
      }
      only_root[only_roots] = (int)root_id;
      only_count[only_roots++] = listed;
    }
    fclose(only_file);
  }
  pick_done =
      calloc_or_die((size_t)(num_picks > 0 ? num_picks : 1), sizeof(bool));
  char *out_path = get_formatted_string("%s.peg2.csv", prefix);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id, code;
      if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
        for (int idx = 0; idx < num_picks; idx++) {
          if (pick_root[idx] == root_id && pick_code[idx] == code) {
            pick_done[idx] = true;
          }
        }
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output,
            "root,code,choice,tiles,pass,drawn,final,win,proven,solve_ms,"
            "reply_ms,reply_turns,pick_ms,net_calls,net_ms,net_checks,"
            "net_hits,move\n");
  }
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  Game *swap = game_duplicate(root);
  Peg1NetChooser *net = peg1_net_chooser_create(
      string_splitter_get_item(fields, 7), string_splitter_get_item(fields, 8),
      atoi(string_splitter_get_item(fields, 9)),
      strtod(string_splitter_get_item(fields, 10), NULL),
      strtod(string_splitter_get_item(fields, 11), NULL), root, 2);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  PegExactSolver exact = peg_exact_create(plies, tt_fraction);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool pending = false;
    for (int idx = 0; idx < num_picks; idx++) {
      pending |= pick_root[idx] == root_id && !pick_done[idx];
    }
    if (!pending) {
      continue;
    }
    const int seat = game_get_player_on_turn_index(root);
    int unseen[VALUE_NET_TILE_TYPES];
    if (peg_world_public_unseen(root, unseen) != PEG2_UNSEEN) {
      log_fatal("peg2eval root does not have nine unseen tiles");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int pass_rank;
    const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg2eval candidate replay mismatch");
    }
    const Move *only_moves[PEG2_EVAL_ONLY_MAX];
    int n_only = 0;
    if (only_root != NULL) {
      int entry = -1;
      for (int idx = 0; idx < only_roots && entry < 0; idx++) {
        if (only_root[idx] == root_id) {
          entry = idx;
        }
      }
      if (entry < 0) {
        log_fatal("peg2eval root missing from PEG2_EVAL_ONLY");
      }
      for (int idx = 0; idx < only_count[entry]; idx++) {
        const int rank = only_rank[entry * PEG2_EVAL_ONLY_MAX + idx];
        if (rank >= count) {
          log_fatal("peg2eval PEG2_EVAL_ONLY rank outside the pool");
        }
        only_moves[n_only++] = &candidates[rank];
      }
    }
    // The actual bag tiles, in a fixed per-board draw order.
    MachineLetter bag2[2];
    int found = 0;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      for (int copy = 0;
           copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml);
           copy++) {
        bag2[found++] = (MachineLetter)ml;
      }
    }
    if (found != 2) {
      log_fatal("peg2eval bag does not hold two tiles");
    }
    if (splitmix64((uint64_t)root_id) & 1) {
      const MachineLetter first = bag2[0];
      bag2[0] = bag2[1];
      bag2[1] = first;
    }
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || pick_done[idx]) {
        continue;
      }
      const int code = pick_code[idx];
      int64_t calls0, checks0;
      double net_ms0;
      peg1_net_chooser_totals(net, &calls0, &checks0, &net_ms0);
      const int64_t hits0 = peg1_net_chooser_cache_hits(net);
      Move pick;
      double pick_ms = 0;
      if (code >= 0) {
        // Codes past the static pool are wide ranks.
        if (code < count) {
          move_copy(&pick, &candidates[code]);
        } else {
          const int wide_count =
              peg_world_all_placements(root, wide, wide_moves);
          if (code >= wide_count) {
            log_fatal("peg2eval invalid candidate");
          }
          move_copy(&pick, &wide_moves[code]);
        }
      } else if (code == PEG2_PICK_DEFAULT_PEG) {
        default_peg_pick_only(root, 1, NULL, NULL, only_moves, n_only, &pick,
                              &pick_ms);
      } else if (code == PEG2_PICK_NET_PEG ||
                 code == PEG2_PICK_NET_PEG_CHECKED ||
                 code == PEG2_PICK_NET_PEG_UNCACHED) {
        net->check = code == PEG2_PICK_NET_PEG_CHECKED;
        net->cache = code != PEG2_PICK_NET_PEG_UNCACHED;
        peg1_net_chooser_reset(net);
        default_peg_pick_only(root, 1, peg1_net_chooser_callback, net,
                              only_moves, n_only, &pick, &pick_ms);
        if (net->check) {
          // Swap one bag tile with a tile from the opponent's rack: the
          // mover's information is unchanged, so the pick must be too.
          const Rack *opp_rack =
              player_get_rack(game_get_player(root, 1 - seat));
          int other = -1;
          for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
            if (ml != bag2[0] && rack_get_letter(opp_rack, ml) > 0) {
              other = ml;
            }
          }
          if (other >= 0) {
            game_copy(swap, root);
            Rack *swap_rack = player_get_rack(game_get_player(swap, 1 - seat));
            rack_take_letter(swap_rack, (MachineLetter)other);
            rack_add_letter(swap_rack, bag2[0]);
            const MachineLetter swapped_bag[2] = {(MachineLetter)other,
                                                  bag2[1]};
            bag_set_to_tiles(game_get_bag(swap), swapped_bag, 2);
            Move private_pick;
            double ignored;
            peg1_net_chooser_reset(net);
            // The swapped game replays the same pool (the mover's rack and
            // board are unchanged), so the restricted moves still apply.
            default_peg_pick_only(swap, 1, peg1_net_chooser_callback, net,
                                  only_moves, n_only, &private_pick, &ignored);
            if (compare_moves(&pick, &private_pick, true) != -1) {
              log_fatal("network PEG pick depends on the hidden tiles");
            }
          }
          net->check = false;
        }
        // The continuation's reply policy does not use the cache.
        net->cache = false;
      } else {
        log_fatal("peg2eval invalid pick code");
      }
      int choice = -1;
      for (int cand = 0; cand < count && choice < 0; cand++) {
        if (compare_moves(&pick, &candidates[cand], true) == -1) {
          choice = cand;
        }
      }
      int64_t calls1, checks1;
      double net_ms1;
      peg1_net_chooser_totals(net, &calls1, &checks1, &net_ms1);
      const int64_t hits1 = peg1_net_chooser_cache_hits(net);
      const int tiles = move_get_tiles_played(&pick);
      const int n_drawn = tiles < 2 ? tiles : 2;
      peg_world_after_candidate(root, unseen, &pick, bag2, n_drawn, game);
      double reply_ms = 0;
      const int reply_turns =
          peg2_play_drawing_turns(game, net, reply, static_list, &reply_ms);
      bool proven = true;
      double solve_ms = 0;
      double final = spread_for_player(game, seat);
      if (!game_over(game)) {
        if (bag_get_letters(game_get_bag(game)) != 0) {
          log_fatal("peg2eval bag not empty before the exact solve");
        }
        const int on_turn = game_get_player_on_turn_index(game);
        const int64_t start = ctimer_monotonic_ns();
        const int value = peg_exact_value(&exact, game, &proven);
        solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        final += on_turn == seat ? value : -value;
        if (exact.capped) {
          char *capped_path = get_formatted_string("%s.capped.csv", prefix);
          FILE *capped = fopen_or_die(capped_path, "a");
          free(capped_path);
          fprintf(capped, "%d,%d,%d,%d,%.9g\n", root_id, code, exact.depth,
                  value, solve_ms);
          fclose(capped);
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output,
              "%d,%d,%d,%d,%d,%d,%.9g,%.1f,%d,%.9g,%.9g,%d,%.9g,%lld,%.9g,%"
              "lld,%lld,\"%s\"\n",
              root_id, code, choice, tiles,
              move_get_type(&pick) == GAME_EVENT_PASS,
              n_drawn == 1 ? bag2[0] : -1, final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              proven, solve_ms, reply_ms, reply_turns, pick_ms,
              (long long)(calls1 - calls0), net_ms1 - net_ms0,
              (long long)(checks1 - checks0), (long long)(hits1 - hits0),
              string_builder_peek(name));
      fflush(output);
      fsync(fileno(output));
      pick_done[idx] = true;
    }
  }
  free(line);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  peg_exact_destroy(&exact);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  move_list_destroy(wide);
  free(wide_moves);
  peg1_net_chooser_destroy(net);
  game_destroy(game);
  game_destroy(swap);
  config_destroy(config);
  free(pick_root);
  free(pick_code);
  free(pick_done);
  free(only_root);
  free(only_count);
  free(only_rank);
}

void peg2_pick_eval_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 9) {
    log_fatal("peg2pick needs positions prefix, output prefix, model, first "
              "model|-, stage N, top K, check boards, dump");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  Peg2CandidateNet *model =
      peg2_candidate_net_load(string_splitter_get_item(fields, 3));
  if (model->inputs != PEG2_NET_INPUT) {
    log_fatal("peg2pick model must use the full cheap input");
  }
  const char *first_path = string_splitter_get_item(fields, 4);
  Peg2CandidateNet *first = strings_equal(first_path, "-")
                                ? NULL
                                : peg2_candidate_net_load(first_path);
  if (first != NULL && first->inputs != PEG2_NET_FIRST_STAGE_INPUT) {
    log_fatal("peg2pick first model must use the opponent-reply input");
  }
  const int stage_n = atoi(string_splitter_get_item(fields, 5));
  int top_k = atoi(string_splitter_get_item(fields, 6));
  if (top_k <= 0) {
    top_k = model->top_k;
  }
  if (top_k > PEG_STATIC_POOL_SIZE || (first != NULL && stage_n < 1)) {
    log_fatal("peg2pick invalid top K or stage N");
  }
  const int check_boards = atoi(string_splitter_get_item(fields, 7));
  const bool dump_rows = atoi(string_splitter_get_item(fields, 8)) != 0;
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Game *swapped_root = game_duplicate(root);
  Peg2PickScratch work = peg2_pick_scratch_create(root);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.picks.csv", prefix);
  FILE *picks = fopen_exclusive_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.scores.csv", prefix);
  FILE *scores_out = fopen_exclusive_or_die(path, "w");
  free(path);
  FILE *dump = NULL;
  if (dump_rows) {
    path = get_formatted_string("%s.worlds.f32", prefix);
    dump = fopen_exclusive_or_die(path, "w");
    free(path);
  }
  fprintf(picks, "root,candidates,scored,pick,tiles,pool_ms,prep_ms,opp_ms,"
                 "greedy_ms,model_ms,total_ms,checked\n");
  fprintf(scores_out, "root,candidate,first_score,score\n");
  path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  Move candidates[PEG_STATIC_POOL_SIZE], candidates2[PEG_STATIC_POOL_SIZE];
  float scores[PEG_STATIC_POOL_SIZE], scores2[PEG_STATIC_POOL_SIZE];
  float first_scores[PEG_STATIC_POOL_SIZE], first_scores2[PEG_STATIC_POOL_SIZE];
  int boards = 0;
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    const int mover = game_get_player_on_turn_index(root);
    Peg2PickTiming timing = {0};
    int count, scored;
    const int64_t start = ctimer_monotonic_ns();
    const int pick = peg2_net_staged_pick(root, model, first, stage_n, top_k,
                                          &work, candidates, &count, &scored,
                                          scores, first_scores, &timing, dump);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count != expected) {
      log_fatal("peg2pick candidate replay mismatch");
    }
    bool checked = false;
    if (boards++ < check_boards) {
      int actual[2] = {-1, -1}, found = 0;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        for (int copy = 0;
             copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml) &&
             found < 2;
             copy++) {
          actual[found++] = ml;
        }
      }
      int full_unseen[VALUE_NET_TILE_TYPES], rest[VALUE_NET_TILE_TYPES];
      peg_world_public_unseen(root, full_unseen);
      memcpy(rest, full_unseen, sizeof(rest));
      rest[actual[0]]--;
      rest[actual[1]]--;
      MachineLetter swap2[2];
      int picked = 0;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && picked < 2; ml++) {
        while (rest[ml] > 0 && picked < 2 && ml != actual[0] &&
               ml != actual[1]) {
          swap2[picked++] = (MachineLetter)ml;
          rest[ml]--;
        }
      }
      if (picked == 2) {
        Move pass;
        move_set_as_pass(&pass);
        peg_world_after_candidate(root, full_unseen, &pass, swap2, 0,
                                  swapped_root);
        bag_set_to_tiles(game_get_bag(swapped_root), swap2, 2);
        game_set_player_on_turn_index(swapped_root, mover);
        Peg2PickTiming ignored = {0};
        int count2, scored2;
        const int pick2 = peg2_net_staged_pick(
            swapped_root, model, first, stage_n, top_k, &work, candidates2,
            &count2, &scored2, scores2, first_scores2, &ignored, NULL);
        if (pick2 != pick || scored2 != scored ||
            memcmp(scores, scores2, sizeof(float) * (size_t)scored) != 0 ||
            (first != NULL && memcmp(first_scores, first_scores2,
                                     sizeof(float) * (size_t)scored) != 0)) {
          log_fatal("peg2pick depends on hidden tiles");
        }
        checked = true;
      }
    }
    fprintf(picks, "%d,%d,%d,%d,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%d\n", root_id,
            count, scored, pick,
            pick >= 0 ? move_get_tiles_played(&candidates[pick]) : 0,
            (double)timing.pool_ns / 1e6, (double)timing.prep_ns / 1e6,
            (double)timing.opp_ns / 1e6, (double)timing.greedy_ns / 1e6,
            (double)timing.model_ns / 1e6, total_ms, checked);
    for (int rank = 0; rank < scored; rank++) {
      fprintf(scores_out, "%d,%d,%.9g,%.9g\n", root_id, rank,
              first != NULL ? first_scores[rank] : NAN, scores[rank]);
    }
    fflush(picks);
    fflush(scores_out);
  }
  free(line);
  fclose(positions);
  fclose(picks);
  fclose(scores_out);
  if (dump != NULL) {
    fclose(dump);
  }
  error_stack_destroy(errors);
  peg2_pick_scratch_destroy(&work);
  peg2_candidate_net_destroy(model);
  if (first != NULL) {
    peg2_candidate_net_destroy(first);
  }
  game_destroy(swapped_root);
  config_destroy(config);
}

void peg2_wide_pick_eval_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 8) {
    log_fatal("peg2wide needs positions prefix, output prefix, first model, "
              "full model|-, stage N, check boards, dump");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  Peg2CandidateNet *first =
      peg2_candidate_net_load(string_splitter_get_item(fields, 3));
  if (first->inputs != PEG2_NET_FIRST_STAGE_INPUT) {
    log_fatal("peg2wide first model must use the opponent-reply input");
  }
  const char *model_path = string_splitter_get_item(fields, 4);
  Peg2CandidateNet *model = strings_equal(model_path, "-")
                                ? NULL
                                : peg2_candidate_net_load(model_path);
  if (model != NULL && model->inputs != PEG2_NET_INPUT) {
    log_fatal("peg2wide full model must use the full cheap input");
  }
  const int stage_n = atoi(string_splitter_get_item(fields, 5));
  if (model != NULL && stage_n < 1) {
    log_fatal("peg2wide invalid stage N");
  }
  const int check_boards = atoi(string_splitter_get_item(fields, 6));
  const bool dump_all = atoi(string_splitter_get_item(fields, 7)) != 0;
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Game *swapped_root = game_duplicate(root);
  Peg2PickScratch work = peg2_pick_scratch_create(root);
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *moves2 = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  bool *taken = malloc_or_die(sizeof(bool) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  float *first_scores =
      malloc_or_die(sizeof(float) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  float *scores = malloc_or_die(sizeof(float) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  float *first_scores2 =
      malloc_or_die(sizeof(float) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  float *scores2 = malloc_or_die(sizeof(float) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.picks.csv", prefix);
  FILE *picks = fopen_exclusive_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.ranks.csv", prefix);
  FILE *ranks_out = fopen_exclusive_or_die(path, "w");
  free(path);
  fprintf(picks, "root,candidates,pick,tiles,pool_ms,prep_ms,opp_ms,"
                 "greedy_ms,model_ms,total_ms,checked\n");
  fprintf(ranks_out,
          "root,candidate,tiles,static_equity,first_score,score,move\n");
  path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  int boards = 0;
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    Peg2PickTiming timing = {0};
    int count;
    const int64_t start = ctimer_monotonic_ns();
    const int pick =
        peg2_net_wide_pick(root, first, model, stage_n, &work, wide, moves,
                           taken, &count, first_scores, scores, &timing);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count < expected) {
      log_fatal("peg2wide move list shorter than the static pool");
    }
    bool checked = false;
    if (boards++ < check_boards &&
        peg_world_swap_hidden_tiles(root, swapped_root)) {
      Peg2PickTiming ignored = {0};
      int count2;
      const int pick2 = peg2_net_wide_pick(swapped_root, first, model, stage_n,
                                           &work, wide, moves2, taken, &count2,
                                           first_scores2, scores2, &ignored);
      if (pick2 != pick || count2 != count ||
          memcmp(first_scores, first_scores2, sizeof(float) * (size_t)count) !=
              0 ||
          memcmp(scores, scores2, sizeof(float) * (size_t)count) != 0) {
        log_fatal("peg2wide depends on hidden tiles");
      }
      checked = true;
    }
    fprintf(picks, "%d,%d,%d,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%d\n", root_id,
            count, pick, pick >= 0 ? move_get_tiles_played(&moves[pick]) : 0,
            (double)timing.pool_ns / 1e6, (double)timing.prep_ns / 1e6,
            (double)timing.opp_ns / 1e6, (double)timing.greedy_ns / 1e6,
            (double)timing.model_ns / 1e6, total_ms, checked);
    for (int rank = 0; rank < count; rank++) {
      if (!dump_all && isnan(scores[rank]) && rank != pick) {
        continue;
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &moves[rank],
                              game_get_ld(root), false);
      fprintf(ranks_out, "%d,%d,%d,%.9g,%.9g,%.9g,\"%s\"\n", root_id, rank,
              move_get_tiles_played(&moves[rank]),
              equity_to_double(move_get_equity(&moves[rank])),
              first_scores[rank], scores[rank], string_builder_peek(name));
    }
    fflush(picks);
    fflush(ranks_out);
  }
  free(line);
  fclose(positions);
  fclose(picks);
  fclose(ranks_out);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  peg2_pick_scratch_destroy(&work);
  move_list_destroy(wide);
  free(moves);
  free(moves2);
  free(taken);
  free(first_scores);
  free(scores);
  free(first_scores2);
  free(scores2);
  peg2_candidate_net_destroy(first);
  if (model != NULL) {
    peg2_candidate_net_destroy(model);
  }
  game_destroy(swapped_root);
  config_destroy(config);
}

// ---- 3PEG -------------------------------------------------------------------
// Three tiles in the bag, mover on turn, ten tiles unseen to the mover.
// peg3eval's pick codes besides the static ranks:
enum {
  PEG3_UNSEEN = RACK_SIZE + 3,
  // PEG with the 1PEG network and the 2PEG chooser at nested nodes.
  PEG3_PICK_NETS_PEG = -7,
  // The same with the hidden-tile checks (both choosers and the root pick).
  PEG3_PICK_NETS_PEG_CHECKED = -8,
  // Default PEG over every root placement, without the pass.
  PEG3_PICK_DEFAULT_PEG_NO_PASS = -3,
};

void peg3_exact_eval_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 16) {
    log_fatal("peg3eval needs positions prefix, choices, output prefix, "
              "reply, plies, tt fraction, first, second|-, shortlist, "
              "w_spread, scale, 2PEG model, 2PEG first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const char *reply_name = string_splitter_get_item(fields, 4);
  const int plies = atoi(string_splitter_get_item(fields, 5));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 6), NULL);
  Peg2ReplyPolicy reply;
  if (strings_equal(reply_name, "net")) {
    reply = PEG2_REPLY_NET;
  } else if (strings_equal(reply_name, "peg")) {
    reply = PEG2_REPLY_PEG;
  } else {
    log_fatal("peg3eval reply must be net or peg");
  }
  if (plies < 1 || plies > 25 || tt_fraction <= 0 || tt_fraction > 0.1) {
    log_fatal("peg3eval invalid plies or tt fraction");
  }
  const char *depth_env = getenv("PEG3_EVAL_NEST_DEPTH");
  const int nest_depth = depth_env != NULL && atoi(depth_env) > 0
                             ? atoi(depth_env)
                             : PEG_NESTED_DEFAULT_DEPTH;
  // PEG3_EVAL_THREADS > 1 runs each root PEG pick on that many threads. PEG
  // hands the choosers worker indices 0..threads (the main thread helps the
  // pool), so they get threads + 1 slots.
  const char *threads_env = getenv("PEG3_EVAL_THREADS");
  const int pick_threads =
      threads_env != NULL && atoi(threads_env) > 1 ? atoi(threads_env) : 1;
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * pick_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *choices = fopen_or_die(choices_path, "r");
  while (getline_ignore_carriage_return(&line, &capacity, choices) >= 0) {
    int root_id, code;
    if (sscanf(line, "%d,%d", &root_id, &code) != 2) {
      continue;
    }
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root = realloc_or_die(pick_root, sizeof(int) * pick_capacity);
      pick_code = realloc_or_die(pick_code, sizeof(int) * pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks++] = code;
  }
  fclose(choices);
  bool *pick_done =
      calloc_or_die((size_t)(num_picks > 0 ? num_picks : 1), sizeof(bool));
  char *out_path = get_formatted_string("%s.peg3.csv", prefix);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id, code;
      if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
        for (int idx = 0; idx < num_picks; idx++) {
          if (pick_root[idx] == root_id && pick_code[idx] == code) {
            pick_done[idx] = true;
          }
        }
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output, "root,code,choice,tiles,pass,final,win,proven,solve_ms,"
                    "reply_ms,reply_turns,static_turns,pick_ms,net1_calls,"
                    "net1_hits,net1_ms,net2_calls,net2_hits,net2_ms,checks,"
                    "nest_depth,move\n");
  }
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  Game *swap = game_duplicate(root);
  Peg1NetChooser *net = peg1_net_chooser_create(
      string_splitter_get_item(fields, 7), string_splitter_get_item(fields, 8),
      atoi(string_splitter_get_item(fields, 9)),
      strtod(string_splitter_get_item(fields, 10), NULL),
      strtod(string_splitter_get_item(fields, 11), NULL), root,
      pick_threads + 1);
  Peg2NetChooser *chooser = peg2_net_chooser_create(
      string_splitter_get_item(fields, 12),
      string_splitter_get_item(fields, 13),
      atoi(string_splitter_get_item(fields, 14)),
      atoi(string_splitter_get_item(fields, 15)), root, pick_threads + 1);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  const Move **placements =
      malloc_or_die(sizeof(Move *) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  PegExactSolver exact = peg_exact_create(plies, tt_fraction);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 3, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool pending = false;
    for (int idx = 0; idx < num_picks; idx++) {
      pending |= pick_root[idx] == root_id && !pick_done[idx];
    }
    if (!pending) {
      continue;
    }
    const int seat = game_get_player_on_turn_index(root);
    int unseen[VALUE_NET_TILE_TYPES];
    if (peg_world_public_unseen(root, unseen) != PEG3_UNSEEN) {
      log_fatal("peg3eval root does not have ten unseen tiles");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int pass_rank;
    const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg3eval candidate replay mismatch");
    }
    const int n_placements = peg_world_all_placements(root, wide, wide_moves);
    for (int rank = 0; rank < n_placements; rank++) {
      placements[rank] = &wide_moves[rank];
    }
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || pick_done[idx]) {
        continue;
      }
      const int code = pick_code[idx];
      int64_t calls1_0, checks1_0, calls2_0, checks2_0, hits2_0;
      double ms1_0, ms2_0;
      peg1_net_chooser_totals(net, &calls1_0, &checks1_0, &ms1_0);
      peg2_net_chooser_totals(chooser, &calls2_0, &checks2_0, &ms2_0, &hits2_0);
      const int64_t hits1_0 = peg1_net_chooser_cache_hits(net);
      Move pick;
      double pick_ms = 0;
      int depth_used = PEG_NESTED_DEFAULT_DEPTH;
      int root_checks = 0;
      if (code >= 0) {
        if (code >= count) {
          log_fatal("peg3eval invalid candidate");
        }
        move_copy(&pick, &candidates[code]);
        depth_used = 0;
      } else if (code == PEG2_PICK_DEFAULT_PEG) {
        default_peg_pick(root, pick_threads, NULL, NULL, &pick, &pick_ms);
      } else if (code == PEG3_PICK_DEFAULT_PEG_NO_PASS) {
        default_peg_pick_only(root, pick_threads, NULL, NULL, placements,
                              n_placements, &pick, &pick_ms);
      } else if (code == PEG2_PICK_NET_PEG || code == PEG3_PICK_NETS_PEG ||
                 code == PEG3_PICK_NETS_PEG_CHECKED) {
        const bool both = code != PEG2_PICK_NET_PEG;
        const bool check = code == PEG3_PICK_NETS_PEG_CHECKED;
        net->check = check;
        net->cache = true;
        chooser->check = check;
        chooser->cache = true;
        depth_used = nest_depth;
        peg1_net_chooser_reset(net);
        peg2_net_chooser_reset(chooser);
        default_peg_pick_with_nets(
            root, pick_threads, peg1_net_chooser_callback, net,
            both ? peg2_net_chooser_callback : NULL, both ? chooser : NULL,
            nest_depth, placements, n_placements, &pick, &pick_ms);
        if (check) {
          // Swap one bag tile with a tile from the opponent's rack: the
          // mover's information is unchanged, so the pick must be too.
          MachineLetter bag3[3];
          const int held = bag_peek_tiles(game_get_bag(root), bag3);
          const Rack *opp_rack =
              player_get_rack(game_get_player(root, 1 - seat));
          int other = -1;
          for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
            if (ml != bag3[0] && rack_get_letter(opp_rack, ml) > 0) {
              other = ml;
            }
          }
          if (held == 3 && other >= 0) {
            game_copy(swap, root);
            Rack *swap_rack = player_get_rack(game_get_player(swap, 1 - seat));
            rack_take_letter(swap_rack, (MachineLetter)other);
            rack_add_letter(swap_rack, bag3[0]);
            const MachineLetter swapped_bag[3] = {(MachineLetter)other, bag3[1],
                                                  bag3[2]};
            bag_set_to_tiles(game_get_bag(swap), swapped_bag, 3);
            Move private_pick;
            double ignored;
            peg1_net_chooser_reset(net);
            peg2_net_chooser_reset(chooser);
            default_peg_pick_with_nets(
                swap, pick_threads, peg1_net_chooser_callback, net,
                peg2_net_chooser_callback, chooser, nest_depth, placements,
                n_placements, &private_pick, &ignored);
            if (compare_moves(&pick, &private_pick, true) != -1) {
              log_fatal("network 3PEG pick depends on the hidden tiles");
            }
            root_checks = 1;
          }
        }
        net->check = false;
        chooser->check = false;
        // The continuation's reply policy does not use the cache.
        net->cache = false;
        chooser->cache = false;
      } else {
        log_fatal("peg3eval invalid pick code");
      }
      int64_t calls1_1, checks1_1, calls2_1, checks2_1, hits2_1;
      double ms1_1, ms2_1;
      peg1_net_chooser_totals(net, &calls1_1, &checks1_1, &ms1_1);
      peg2_net_chooser_totals(chooser, &calls2_1, &checks2_1, &ms2_1, &hits2_1);
      const int64_t hits1_1 = peg1_net_chooser_cache_hits(net);
      int choice = -1;
      for (int cand = 0; cand < count && choice < 0; cand++) {
        if (compare_moves(&pick, &candidates[cand], true) == -1) {
          choice = cand;
        }
      }
      game_copy(game, root);
      play_move(&pick, game, NULL);
      double reply_ms = 0;
      int static_turns = 0;
      const int reply_turns = peg3_play_drawing_turns(
          game, net, chooser, 0, reply, static_list, &reply_ms, &static_turns);
      bool proven = true;
      double solve_ms = 0;
      double final = spread_for_player(game, seat);
      if (!game_over(game)) {
        if (bag_get_letters(game_get_bag(game)) != 0) {
          log_fatal("peg3eval bag not empty before the exact solve");
        }
        const int on_turn = game_get_player_on_turn_index(game);
        const int64_t start = ctimer_monotonic_ns();
        const int value = peg_exact_value(&exact, game, &proven);
        solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        final += on_turn == seat ? value : -value;
        if (exact.capped) {
          char *capped_path = get_formatted_string("%s.capped.csv", prefix);
          FILE *capped = fopen_or_die(capped_path, "a");
          free(capped_path);
          fprintf(capped, "%d,%d,%d,%d,%.9g\n", root_id, code, exact.depth,
                  value, solve_ms);
          fclose(capped);
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output,
              "%d,%d,%d,%d,%d,%.9g,%.1f,%d,%.9g,%.9g,%d,%d,%.9g,%lld,%lld,%"
              ".9g,%lld,%lld,%.9g,%lld,%d,\"%s\"\n",
              root_id, code, choice, move_get_tiles_played(&pick),
              move_get_type(&pick) == GAME_EVENT_PASS, final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              proven, solve_ms, reply_ms, reply_turns, static_turns, pick_ms,
              (long long)(calls1_1 - calls1_0), (long long)(hits1_1 - hits1_0),
              ms1_1 - ms1_0, (long long)(calls2_1 - calls2_0),
              (long long)(hits2_1 - hits2_0), ms2_1 - ms2_0,
              (long long)(checks1_1 - checks1_0 + checks2_1 - checks2_0 +
                          root_checks),
              depth_used, string_builder_peek(name));
      fflush(output);
      fsync(fileno(output));
      pick_done[idx] = true;
    }
  }
  free(line);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  peg_exact_destroy(&exact);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  move_list_destroy(wide);
  free(wide_moves);
  free(placements);
  peg1_net_chooser_destroy(net);
  peg2_net_chooser_destroy(chooser);
  game_destroy(game);
  game_destroy(swap);
  config_destroy(config);
  free(pick_root);
  free(pick_code);
  free(pick_done);
}

// ---- Late-game sims with the PEG choosers in the rollouts -------------------
// Per-worker fixed-depth endgame search for rollouts (the bag is empty, so
// both racks are known in the rollout's world): fixed_depth_endgame_move's
// search with each worker's solver context kept between calls.
// PEG_SIM_EG_TT: the searches' transposition table. "none" (the default):
// every search starts empty; "worker": one table per worker, kept across that
// worker's searches; "shared": one table for every worker. Tables hold
// 2^PEG_SIM_EG_TT_POWER entries (default 26 shared, 24 per worker) and are
// cleared before each sim (one root and arm). A kept table makes a rollout's
// result depend on the searches before it, so runs are not reproducible.
typedef struct PegSimRolloutEndgame {
  int plies;
  int workers;
  EndgameCtx **ctx;
  EndgameResults **results;
  ThreadControl **control;
  int64_t *calls;
  double *ms;
  int n_tt;
  TranspositionTable **tt;
} PegSimRolloutEndgame;

static PegSimRolloutEndgame peg_sim_endgame_create(int plies, int workers) {
  PegSimRolloutEndgame endgame = {
      .plies = plies,
      .workers = workers,
      .ctx = calloc_or_die((size_t)workers, sizeof(EndgameCtx *)),
      .results = malloc_or_die(sizeof(EndgameResults *) * (size_t)workers),
      .control = malloc_or_die(sizeof(ThreadControl *) * (size_t)workers),
      .calls = calloc_or_die((size_t)workers, sizeof(int64_t)),
      .ms = calloc_or_die((size_t)workers, sizeof(double)),
  };
  for (int worker = 0; worker < workers; worker++) {
    endgame.results[worker] = endgame_results_create();
    endgame.control[worker] = thread_control_create();
  }
  return endgame;
}

static void peg_sim_endgame_set_tt(PegSimRolloutEndgame *endgame) {
  const char *mode = getenv("PEG_SIM_EG_TT");
  if (mode == NULL || strings_equal(mode, "none")) {
    return;
  }
  const bool shared = strings_equal(mode, "shared");
  if (!shared && !strings_equal(mode, "worker")) {
    log_fatal("PEG_SIM_EG_TT must be none, worker or shared");
  }
  const char *power_env = getenv("PEG_SIM_EG_TT_POWER");
  const int power = power_env != NULL ? atoi(power_env) : shared ? 26 : 24;
  if (power < TT_MIN_SIZE_POWER || power > 32) {
    log_fatal("PEG_SIM_EG_TT_POWER must be %d..32", TT_MIN_SIZE_POWER);
  }
  // transposition_table_create takes a fraction of the search memory budget
  // and rounds the entry count down to a power of two.
  const double fraction = ldexp(1.0, power) * TTENTRY_SIZE_BYTES * 1.0001 /
                          (double)get_search_memory_budget_base();
  endgame->n_tt = shared ? 1 : endgame->workers;
  endgame->tt =
      malloc_or_die(sizeof(TranspositionTable *) * (size_t)endgame->n_tt);
  for (int idx = 0; idx < endgame->n_tt; idx++) {
    endgame->tt[idx] = transposition_table_create(fraction);
  }
}

static void peg_sim_endgame_clear_tt(PegSimRolloutEndgame *endgame) {
  for (int idx = 0; idx < endgame->n_tt; idx++) {
    transposition_table_reset(endgame->tt[idx]);
  }
}

static void peg_sim_endgame_destroy(PegSimRolloutEndgame *endgame) {
  for (int worker = 0; worker < endgame->workers; worker++) {
    if (endgame->ctx[worker] != NULL) {
      endgame_ctx_destroy(endgame->ctx[worker]);
    }
    endgame_results_destroy(endgame->results[worker]);
    thread_control_destroy(endgame->control[worker]);
  }
  free(endgame->ctx);
  free(endgame->results);
  free(endgame->control);
  free(endgame->calls);
  free(endgame->ms);
  for (int idx = 0; idx < endgame->n_tt; idx++) {
    transposition_table_destroy(endgame->tt[idx]);
  }
  free(endgame->tt);
}

static void peg_sim_endgame_move(PegSimRolloutEndgame *endgame, int worker,
                                 const Game *game, Move *move) {
  const int64_t start = ctimer_monotonic_ns();
  EndgameArgs args = {
      .thread_control = endgame->control[worker],
      .game = game,
      .plies = endgame->plies,
      .tt_fraction_of_mem = 0,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .incremental_movegen = true,
      .enable_pv_display = true,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .shared_tt = endgame->n_tt == 0   ? NULL
                   : endgame->n_tt == 1 ? endgame->tt[0]
                                        : endgame->tt[worker],
  };
  EndgameResults *results = endgame->results[worker];
  endgame_solve_inline(&endgame->ctx[worker], &args, results);
  const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED ||
      endgame_results_get_depth(results, ENDGAME_RESULT_BEST) !=
          endgame->plies ||
      pv->num_moves < 1) {
    log_fatal("pegsim rollout endgame search did not finish");
  }
  small_move_to_move(move, &pv->moves[0], game_get_board(game));
  endgame->calls[worker]++;
  endgame->ms[worker] += (double)(ctimer_monotonic_ns() - start) / 1e6;
}

// PEG_SIM_TRACE=PATH: one line per rollout decision of the hooked arms, for
// measuring how often rollout positions repeat. Columns: root, arm, worker,
// bag, tiles on the board, side on turn, kind (E the endgame search, 1 or 2 a
// chooser's pick, S static), the position key (board, both racks, side on
// turn, scoreless turns: what a bag-empty search depends on), the on-turn
// player's information key (peg_net_cache_key) and microseconds.
typedef struct PegSimTrace {
  FILE *out;
  cpthread_mutex_t mutex;
  int root;
  int arm;
} PegSimTrace;

static uint64_t peg_sim_position_key(const Game *game, int *tiles) {
  uint64_t hash = UINT64_C(1469598103934665603);
  const Board *board = game_get_board(game);
  *tiles = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter ml = board_get_letter(board, row, col);
      *tiles += ml != ALPHABET_EMPTY_SQUARE_MARKER;
      hash = (hash ^ ml) * UINT64_C(1099511628211);
    }
  }
  for (int player = 0; player < 2; player++) {
    const Rack *rack = player_get_rack(game_get_player(game, player));
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      hash = (hash ^ (uint64_t)rack_get_letter(rack, (MachineLetter)ml)) *
             UINT64_C(1099511628211);
    }
  }
  hash = (hash ^ (uint64_t)game_get_player_on_turn_index(game)) *
         UINT64_C(1099511628211);
  hash = (hash ^ (uint64_t)game_get_consecutive_scoreless_turns(game)) *
         UINT64_C(1099511628211);
  return hash;
}

static void peg_sim_trace_write(PegSimTrace *trace, int worker_index,
                                const Game *game, char kind, int64_t start) {
  const double us = (double)(ctimer_monotonic_ns() - start) / 1e3;
  int tiles;
  const uint64_t position = peg_sim_position_key(game, &tiles);
  uint8_t key[PEG_NET_CACHE_KEY_BYTES];
  const uint64_t info = peg_net_cache_key(game, key);
  cpthread_mutex_lock(&trace->mutex);
  fprintf(trace->out, "%d,%d,%d,%d,%d,%d,%c,%016llx,%016llx,%.1f\n",
          trace->root, trace->arm, worker_index,
          bag_get_letters(game_get_bag(game)), tiles,
          game_get_player_on_turn_index(game), kind,
          (unsigned long long)position, (unsigned long long)info, us);
  cpthread_mutex_unlock(&trace->mutex);
}

// SimArgs.rollout_move_fn for pegsim. With nets, the 1PEG network chooses
// with one tile in the bag and the 2PEG chooser with two (both from the
// on-turn player's information: their rack, the board and the unseen counts);
// with an endgame search, every bag-empty ply is that search's move; every
// other ply is static. Moves are played in the rollout's own world, so the
// rollout's result is that world's.
typedef struct PegSimRolloutPolicy {
  bool nets;
  Peg1NetChooser *net;
  Peg2NetChooser *chooser;
  PegSimRolloutEndgame *endgame;
  PegSimTrace *trace;
} PegSimRolloutPolicy;

static bool peg_sim_rollout_move(void *context, int worker_index,
                                 const Game *game, Move *move) {
  PegSimRolloutPolicy *hook = context;
  const int64_t start = hook->trace != NULL ? ctimer_monotonic_ns() : 0;
  char kind = 'S';
  if (hook->endgame != NULL && !game_over(game) &&
      bag_get_letters(game_get_bag(game)) == 0) {
    peg_sim_endgame_move(hook->endgame, worker_index, game, move);
    kind = 'E';
  } else if (hook->nets &&
             peg1_net_chooser_pick(hook->net, worker_index, game, move)) {
    kind = '1';
  } else if (hook->nets &&
             peg2_net_chooser_pick(hook->chooser, worker_index, game, move)) {
    kind = '2';
  }
  if (hook->trace != NULL) {
    peg_sim_trace_write(hook->trace, worker_index, game, kind, start);
  }
  return kind != 'S';
}

enum {
  PEG_SIM_MAX_ARMS = 6,
};

// Sums of the choosers' per-slot call counts and milliseconds.
static void peg_sim_chooser_totals(const Peg1NetChooser *net,
                                   const Peg2NetChooser *chooser,
                                   int64_t *calls1, double *ms1,
                                   int64_t *calls2, double *ms2) {
  *calls1 = *calls2 = 0;
  *ms1 = *ms2 = 0;
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    *calls1 += net->slot[slot_idx].calls;
    *ms1 += net->slot[slot_idx].ms;
  }
  for (int slot_idx = 0; slot_idx < chooser->slots; slot_idx++) {
    *calls2 += chooser->slot[slot_idx].calls;
    *ms2 += chooser->slot[slot_idx].ms;
  }
}

void peg_sim_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 15) {
    log_fatal("pegsim needs positions prefix, output prefix, candidates, "
              "iterations, plies, first, second|-, shortlist, w_spread, "
              "scale, 2PEG model, 2PEG first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const int max_candidates = atoi(string_splitter_get_item(fields, 3));
  const int iterations = atoi(string_splitter_get_item(fields, 4));
  const int plies = atoi(string_splitter_get_item(fields, 5));
  const char *bag_env = getenv("PEG_SIM_BAG");
  const int bag_size = bag_env != NULL ? atoi(bag_env) : RACK_SIZE;
  const char *threads_env = getenv("PEG_SIM_THREADS");
  const int threads =
      threads_env != NULL && atoi(threads_env) > 1 ? atoi(threads_env) : 1;
  const char *seed_env = getenv("PEG_SIM_SEED");
  const uint64_t base_seed =
      seed_env != NULL ? strtoull(seed_env, NULL, 10) : 0;
  const char *arms_env = getenv("PEG_SIM_ARMS");
  bool run_arm[PEG_SIM_MAX_ARMS] = {false};
  const char *arms_text = arms_env != NULL ? arms_env : "0,1,2";
  for (const char *cursor = arms_text; *cursor != '\0'; cursor++) {
    if (*cursor >= '0' && *cursor < '0' + PEG_SIM_MAX_ARMS) {
      run_arm[*cursor - '0'] = true;
    }
  }
  const char *eg_env = getenv("PEG_SIM_EG_PLIES");
  const int eg_plies = eg_env != NULL ? atoi(eg_env) : 2;
  if (max_candidates < 1 || iterations < 1 || plies < 1 || plies > MAX_PLIES ||
      bag_size < 1 || bag_size > RACK_SIZE || (run_arm[2] && !run_arm[1]) ||
      (run_arm[5] && !run_arm[3]) || eg_plies < 1 || eg_plies > MAX_PLIES) {
    log_fatal("pegsim invalid candidates, iterations, plies, bag, arms or "
              "endgame plies");
  }
  // Roots already in the output are done.
  char *out_path = get_formatted_string("%s.sim.csv", prefix);
  int done_capacity = 64, num_done = 0;
  int *done = malloc_or_die(sizeof(int) * (size_t)done_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *previous = fopen(out_path, "r");
  const bool fresh = previous == NULL;
  if (previous != NULL) {
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) > 0) {
      if (num_done == done_capacity) {
        done_capacity *= 2;
        done = realloc_or_die(done, sizeof(int) * (size_t)done_capacity);
      }
      done[num_done++] = atoi(line);
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  char *cands_path = get_formatted_string("%s.cands.csv", prefix);
  FILE *cands_out = fopen_or_die(cands_path, "a");
  free(cands_path);
  if (fresh) {
    fprintf(output, "root,arm,rank,candidates,utility,win,equity,iterations,"
                    "ms,net1_calls,net1_ms,net2_calls,net2_ms,eg_calls,eg_ms,"
                    "move\n");
    fprintf(cands_out, "root,arm,rank,samples,utility,win,equity\n");
  }
  Config *config = peg_net_config_create();
  ErrorStack *errors = error_stack_create();
  config_load_win_pcts(config, errors);
  if (!error_stack_is_empty(errors)) {
    error_stack_print_and_reset(errors);
    log_fatal("pegsim could not load the win percentages");
  }
  Game *root = config_get_game(config);
  Peg1NetChooser *net = peg1_net_chooser_create(
      string_splitter_get_item(fields, 6), string_splitter_get_item(fields, 7),
      atoi(string_splitter_get_item(fields, 8)),
      strtod(string_splitter_get_item(fields, 9), NULL),
      strtod(string_splitter_get_item(fields, 10), NULL), root, threads);
  Peg2NetChooser *chooser = peg2_net_chooser_create(
      string_splitter_get_item(fields, 11),
      string_splitter_get_item(fields, 12),
      atoi(string_splitter_get_item(fields, 13)),
      atoi(string_splitter_get_item(fields, 14)), root, threads);
  net->cache = true;
  chooser->cache = true;
  const char *check_env = getenv("PEG_SIM_CHECK");
  net->check = check_env != NULL && atoi(check_env) != 0;
  chooser->check = net->check;
  PegSimRolloutEndgame endgame = peg_sim_endgame_create(eg_plies, threads);
  peg_sim_endgame_set_tt(&endgame);
  PegSimRolloutPolicy hooks[PEG_SIM_MAX_ARMS] = {
      {.nets = false},
      {.nets = true, .net = net, .chooser = chooser},
      {.nets = false},
      {.nets = true, .net = net, .chooser = chooser, .endgame = &endgame},
      {.nets = false, .endgame = &endgame},
      {.nets = false},
  };
  PegSimTrace trace = {.out = NULL};
  const char *trace_path = getenv("PEG_SIM_TRACE");
  if (trace_path != NULL) {
    trace.out = fopen_exclusive_or_die(trace_path, "w");
    fprintf(trace.out,
            "root,arm,worker,bag,tiles,on_turn,kind,position,info,us\n");
    cpthread_mutex_init(&trace.mutex);
    for (int arm = 0; arm < PEG_SIM_MAX_ARMS; arm++) {
      hooks[arm].trace = &trace;
    }
  }
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  MoveList *pool = move_list_create(max_candidates);
  SimResults *results = sim_results_create(0.0);
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id =
        peg_positions_next_root(positions, &line, &capacity, root, errors,
                                bag_size, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool skip = false;
    for (int idx = 0; idx < num_done; idx++) {
      skip |= done[idx] == root_id;
    }
    if (skip) {
      continue;
    }
    const int n_placements = peg_world_all_placements(root, wide, wide_moves);
    move_list_reset(pool);
    for (int rank = 0; rank < n_placements && rank < max_candidates; rank++) {
      move_list_add_move(pool, &wide_moves[rank]);
    }
    const int cands = move_list_get_count(pool);
    if (cands == 0) {
      log_fatal("pegsim root %d has no placement", root_id);
    }
    double arm_seconds[PEG_SIM_MAX_ARMS] = {0};
    for (int arm = 0; arm < PEG_SIM_MAX_ARMS; arm++) {
      if (!run_arm[arm]) {
        continue;
      }
      peg1_net_chooser_reset(net);
      peg2_net_chooser_reset(chooser);
      peg_sim_endgame_clear_tt(&endgame);
      trace.root = root_id;
      trace.arm = arm;
      const bool timed = arm == 2 || arm == 5;
      const double time_limit = arm == 2   ? arm_seconds[1]
                                : arm == 5 ? arm_seconds[3]
                                           : 0.0;
      int64_t calls1_before, calls2_before, calls1_after, calls2_after;
      double ms1_before, ms2_before, ms1_after, ms2_after;
      peg_sim_chooser_totals(net, chooser, &calls1_before, &ms1_before,
                             &calls2_before, &ms2_before);
      ThreadControl *control = thread_control_create();
      thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
      SimArgs sim_args;
      const uint64_t max_iterations =
          timed ? UINT64_C(1000000000000000)
                : (uint64_t)iterations * (uint64_t)cands;
      sim_args_fill(plies, pool, cands, NULL, config_get_win_pcts(config), NULL,
                    control, root, false, false, threads, 0, cands, plies,
                    base_seed + (uint64_t)root_id, max_iterations, 1, 0.0,
                    BAI_THRESHOLD_NONE, time_limit,
                    BAI_SAMPLING_RULE_ROUND_ROBIN, 0.0, 1.0, 0.5, 100.0, false,
                    NULL, &sim_args);
      sim_args.bag_cycle = false;
      sim_args.pat_rollout_disabled = true;
      if (hooks[arm].nets || hooks[arm].endgame != NULL) {
        sim_args.rollout_move_fn = peg_sim_rollout_move;
        sim_args.rollout_move_context = &hooks[arm];
      }
      int64_t eg_calls_before = 0;
      double eg_ms_before = 0;
      for (int worker = 0; worker < threads; worker++) {
        eg_calls_before += endgame.calls[worker];
        eg_ms_before += endgame.ms[worker];
      }
      const int64_t start = ctimer_monotonic_ns();
      if (cands > 1) {
        simulate_without_ctx(&sim_args, results, errors);
      }
      const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9;
      if (!error_stack_is_empty(errors)) {
        error_stack_print_and_reset(errors);
        log_fatal("pegsim sim failed on root %d", root_id);
      }
      thread_control_destroy(control);
      arm_seconds[arm] = seconds;
      peg_sim_chooser_totals(net, chooser, &calls1_after, &ms1_after,
                             &calls2_after, &ms2_after);
      int64_t eg_calls = -eg_calls_before;
      double eg_ms = -eg_ms_before;
      for (int worker = 0; worker < threads; worker++) {
        eg_calls += endgame.calls[worker];
        eg_ms += endgame.ms[worker];
      }
      // Each simmed play's static rank, by its text.
      int best_rank = 0;
      double best_utility = NAN, best_win = NAN, best_equity = NAN;
      const int best_index =
          cands > 1 ? sim_results_get_best_move_index(results) : -1;
      for (int play_idx = 0; play_idx < (cands > 1 ? cands : 0); play_idx++) {
        const SimmedPlay *simmed_play =
            sim_results_get_simmed_play(results, play_idx);
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root),
                                simmed_play_get_move(simmed_play),
                                game_get_ld(root), false);
        int rank = -1;
        StringBuilder *other = string_builder_create();
        for (int idx = 0; idx < cands && rank < 0; idx++) {
          string_builder_clear(other);
          string_builder_add_move(other, game_get_board(root),
                                  move_list_get_move(pool, idx),
                                  game_get_ld(root), false);
          if (strings_equal(string_builder_peek(name),
                            string_builder_peek(other))) {
            rank = idx;
          }
        }
        string_builder_destroy(other);
        const Stat *utility = simmed_play_get_utility_stat(simmed_play);
        const double win =
            stat_get_mean(simmed_play_get_win_pct_stat(simmed_play));
        const double equity =
            stat_get_mean(simmed_play_get_equity_stat(simmed_play));
        fprintf(cands_out, "%d,%d,%d,%llu,%.9g,%.9g,%.9g\n", root_id, arm, rank,
                (unsigned long long)stat_get_num_samples(utility),
                stat_get_mean(utility), win, equity);
        if (play_idx == best_index) {
          best_rank = rank;
          best_utility = stat_get_mean(utility);
          best_win = win;
          best_equity = equity;
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root),
                              move_list_get_move(pool, best_rank),
                              game_get_ld(root), false);
      fprintf(
          output,
          "%d,%d,%d,%d,%.9g,%.9g,%.9g,%llu,%.9g,%lld,%.9g,%lld,%.9g,%lld,"
          "%.9g,\"%s\"\n",
          root_id, arm, best_rank, cands, best_utility, best_win, best_equity,
          (unsigned long long)(cands > 1
                                   ? sim_results_get_iteration_count(results)
                                   : 0),
          1000.0 * seconds, (long long)(calls1_after - calls1_before),
          ms1_after - ms1_before, (long long)(calls2_after - calls2_before),
          ms2_after - ms2_before, (long long)eg_calls, eg_ms,
          string_builder_peek(name));
    }
    fflush(cands_out);
    fflush(output);
    fsync(fileno(cands_out));
    fsync(fileno(output));
  }
  free(line);
  free(done);
  fclose(positions);
  fclose(output);
  fclose(cands_out);
  string_builder_destroy(name);
  sim_results_destroy(results);
  move_list_destroy(pool);
  move_list_destroy(wide);
  free(wide_moves);
  error_stack_destroy(errors);
  peg_sim_endgame_destroy(&endgame);
  peg1_net_chooser_destroy(net);
  peg2_net_chooser_destroy(chooser);
  config_destroy(config);
  if (trace.out != NULL) {
    fclose(trace.out);
  }
}

// pegbudget arms (codes in PREFIX.pegb.csv; b = the budget's index).
enum {
  PEG_BUDGET_MAX_BUDGETS = 8,
  PEG_BUDGET_ARM_NET = 0,         // the 2PEG chooser's pick, no search
  PEG_BUDGET_ARM_PEG = 100,       // default PEG (no pass), budget b
  PEG_BUDGET_ARM_PEG_NET = 200,   // PEG with the 1PEG net nested, budget b
  PEG_BUDGET_ARM_NET_ORDER = 300, // PEG with the net nested, over the chooser's
                                  // top K in its order, budget b
  PEG_BUDGET_ARM_HYBRID = 400,    // the chooser's top K in its order, then the
                               // rest of the placements by PEG's greedy stage
                               // 0, first stage PEG_BUDGET_HYBRID_WIDTH wide
  PEG_BUDGET_ARM_HYBRID_UTILITY =
      500,                     // hybrid ranked by utility, no greedy tail
                               // below PEG_BUDGET_HYBRID_GREEDY_MIN_MS
  PEG_BUDGET_ARM_PEG_H2 = 600, // the hybridu search built into PEG: root_ranker
                               // over the chooser, ranked by utility
  PEG_BUDGET_ORDER_K = 16,
  PEG_BUDGET_HYBRID_WIDTH = 48,
  PEG_BUDGET_HYBRID_GREEDY_MIN_MS = 500,
};

// Per-solve progress for pegbudget: first-stage completions (live mode) and
// when the first halving stage began.
typedef struct PegBudgetProgress {
  int stage1_done;
  int64_t start_ns;
  int64_t stage1_ns;
} PegBudgetProgress;

static void peg_budget_on_stage_start(int stage_idx, int k_cands, int inner_d,
                                      int emptier_plies, void *user_data) {
  (void)k_cands;
  (void)inner_d;
  (void)emptier_plies;
  PegBudgetProgress *progress = user_data;
  if (stage_idx == 1 && progress->stage1_ns == 0) {
    progress->stage1_ns = ctimer_monotonic_ns();
  }
}

// Live-mode completions of the first halving stage (PegArgs.on_cand_done).
static void peg_budget_on_cand_done(int stage_idx, int cand_rank,
                                    const Move *cand, double win_pct,
                                    double mean_spread, int scen_done,
                                    int64_t completed_ns, bool reordered,
                                    void *user_data) {
  (void)cand_rank;
  (void)cand;
  (void)win_pct;
  (void)mean_spread;
  (void)scen_done;
  (void)completed_ns;
  (void)reordered;
  if (stage_idx == 1) {
    ((PegBudgetProgress *)user_data)->stage1_done++;
  }
}

// One budgeted root PEG pick: nested 1PEG chooser (NULL = the staged inner
// peg) over only (n_only moves), schedule top_k (NULL = PEG's default),
// ranked by utility or by win% first. With n_ranked > 0 the first n_ranked
// keep their order ahead of the rest and the halving stages run live (one
// candidate at a time). ranker (NULL = none) is PegArgs.root_ranker. Writes
// the pick, wall ms, the last completed stage, whether it was partial, how
// many candidates the first halving stage finished (live only) and when it
// began (ms after the start; -1 = never). PEG_BUDGET_MAX_STAGE > 0 caps the
// halving stages (PegArgs.max_stage), for fixed-work parity checks.
static void peg_budget_pick(const Game *game, int threads,
                            PegPeg1Chooser chooser, void *chooser_ctx,
                            const Move *const *only, int n_only, int n_ranked,
                            const int *top_k, int n_top_k, bool rank_by_utility,
                            PegRootRanker ranker, void *ranker_ctx,
                            double budget_s, Move *best, double *ms, int *stage,
                            bool *partial, int *stage1_done,
                            double *stage1_start_ms) {
  static const int caps[] = {8, 4, 2};
  const char *max_stage_env = getenv("PEG_BUDGET_MAX_STAGE");
  const int max_stage = max_stage_env != NULL ? atoi(max_stage_env) : 0;
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  PegPoll *poll = n_ranked > 0 ? peg_poll_create() : NULL;
  PegBudgetProgress progress = {0};
  PegArgs args;
  peg_args_fill(game, control, threads, budget_s, max_stage,
                /*greedy_seed_only=*/false, top_k, top_k != NULL ? n_top_k : 0,
                /*inner_top_k=*/0, PEG_OPP_RATIONAL, /*scenario_stride=*/0,
                /*nested_enabled=*/true, /*nested_cand_cap=*/0, caps,
                (int)(sizeof(caps) / sizeof(caps[0])), /*nested_stride=*/0,
                /*nested_strides_by_bag=*/NULL, /*nested_n_strides_by_bag=*/0,
                /*nested_emptier_ply_cap=*/0, PEG_NESTED_DEFAULT_DEPTH,
                best_reply_shortcuts_disabled(), chooser, chooser_ctx, NULL,
                NULL,
                /*eval_bag_order=*/NULL, /*eval_bag_order_len=*/0, only, n_only,
                n_ranked, rank_by_utility, ranker, ranker_ctx,
                /*protect_moves=*/NULL,
                /*n_protect_moves=*/0,
                /*include_per_scenario=*/false, peg_budget_on_stage_start,
                n_ranked > 0 || ranker != NULL ? peg_budget_on_cand_done : NULL,
                /*on_scenario_done=*/NULL, &progress, poll,
                /*shared_endgame_tt=*/NULL,
                /*checkpoint=*/NULL, /*resume=*/false, &args);
  PegResult result;
  ErrorStack *errors = error_stack_create();
  progress.start_ns = ctimer_monotonic_ns();
  peg_solve(&args, &result, errors);
  *ms = (double)(ctimer_monotonic_ns() - progress.start_ns) / 1e6;
  if (!error_stack_is_empty(errors) || result.n_top_cands < 1) {
    error_stack_print_and_reset(errors);
    log_fatal("pegbudget peg_solve failed");
  }
  move_copy(best, &result.best_move);
  *stage = result.last_completed_stage;
  *partial = result.last_stage_partial;
  *stage1_done = progress.stage1_done;
  *stage1_start_ms =
      progress.stage1_ns != 0
          ? (double)(progress.stage1_ns - progress.start_ns) / 1e6
          : -1;
  peg_result_destroy(&result);
  error_stack_destroy(errors);
  if (poll != NULL) {
    peg_poll_destroy(poll);
  }
  thread_control_destroy(control);
}

void peg_budget_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 13) {
    log_fatal("pegbudget needs positions prefix, output prefix, budgets, "
              "first, second|-, shortlist, w_spread, scale, 2PEG model, 2PEG "
              "first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  double budgets[PEG_BUDGET_MAX_BUDGETS];
  int n_budgets = 0;
  StringSplitter *budget_items =
      split_string(string_splitter_get_item(fields, 3), ',', true);
  for (int idx = 0; idx < string_splitter_get_number_of_items(budget_items);
       idx++) {
    if (n_budgets == PEG_BUDGET_MAX_BUDGETS) {
      log_fatal("pegbudget takes at most %d budgets", PEG_BUDGET_MAX_BUDGETS);
    }
    budgets[n_budgets] =
        strtod(string_splitter_get_item(budget_items, idx), NULL);
    if (budgets[n_budgets] <= 0) {
      log_fatal("pegbudget budgets must be positive");
    }
    n_budgets++;
  }
  string_splitter_destroy(budget_items);
  const char *threads_env = getenv("PEG_BUDGET_THREADS");
  const int threads =
      threads_env != NULL && atoi(threads_env) > 0 ? atoi(threads_env) : 10;
  const char *arms_env = getenv("PEG_BUDGET_ARMS");
  StringSplitter *arm_items = split_string(
      arms_env != NULL ? arms_env : "net,peg,pegnet,order", ',', true);
  bool arm_net = false, arm_peg = false, arm_peg_net = false, arm_order = false,
       arm_hybrid = false, arm_hybrid_utility = false, arm_peg_h2 = false;
  for (int idx = 0; idx < string_splitter_get_number_of_items(arm_items);
       idx++) {
    const char *item = string_splitter_get_item(arm_items, idx);
    arm_net |= strings_equal(item, "net");
    arm_peg |= strings_equal(item, "peg");
    arm_peg_net |= strings_equal(item, "pegnet");
    arm_order |= strings_equal(item, "order");
    arm_hybrid |= strings_equal(item, "hybrid");
    arm_hybrid_utility |= strings_equal(item, "hybridu");
    arm_peg_h2 |= strings_equal(item, "h2peg");
  }
  string_splitter_destroy(arm_items);
  char *out_path = get_formatted_string("%s.pegb.csv", prefix);
  char *line = NULL;
  size_t capacity = 0;
  int done_capacity = 64, n_done = 0;
  int *done_roots = malloc_or_die(sizeof(int) * (size_t)done_capacity);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id;
      if (sscanf(line, "%d,", &root_id) == 1 &&
          (n_done == 0 || done_roots[n_done - 1] != root_id)) {
        if (n_done == done_capacity) {
          done_capacity *= 2;
          done_roots =
              realloc_or_die(done_roots, sizeof(int) * (size_t)done_capacity);
        }
        done_roots[n_done++] = root_id;
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output, "root,code,arm,budget_s,ms,net_ms,stage,partial,"
                    "stage1_done,s1_ms,move\n");
  }
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Peg1NetChooser *net = peg1_net_chooser_create(
      string_splitter_get_item(fields, 4), string_splitter_get_item(fields, 5),
      atoi(string_splitter_get_item(fields, 6)),
      strtod(string_splitter_get_item(fields, 7), NULL),
      strtod(string_splitter_get_item(fields, 8), NULL), root, threads + 1);
  Peg2NetChooser *chooser = peg2_net_chooser_create(
      string_splitter_get_item(fields, 9), string_splitter_get_item(fields, 10),
      atoi(string_splitter_get_item(fields, 11)),
      atoi(string_splitter_get_item(fields, 12)), root, 1);
  net->cache = true;
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  Peg2NetRanker ranker = {.chooser = chooser,
                          .pool = move_list_create(PEG_STATIC_POOL_SIZE + 1)};
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  const Move **placements =
      malloc_or_die(sizeof(Move *) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  const Move **hybrid =
      malloc_or_die(sizeof(Move *) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool skip = false;
    for (int idx = 0; idx < n_done && !skip; idx++) {
      skip = done_roots[idx] == root_id;
    }
    if (skip) {
      continue;
    }
    int unseen[VALUE_NET_TILE_TYPES];
    if (peg_world_public_unseen(root, unseen) != PEG2_UNSEEN) {
      log_fatal("pegbudget root does not have nine unseen tiles");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int pass_rank;
    const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("pegbudget candidate replay mismatch");
    }
    const int n_placements = peg_world_all_placements(root, wide, wide_moves);
    for (int rank = 0; rank < n_placements; rank++) {
      placements[rank] = &wide_moves[rank];
    }
    // Each row: code, arm name, budget, ms, net ms, stage, partial, done, pick.
    Move pick;
    if (arm_net) {
      const Move *order[PEG_STATIC_POOL_SIZE];
      int pick_rank;
      const int64_t start = ctimer_monotonic_ns();
      peg2_net_chooser_rank(chooser, root, candidates, order, 1, &pick_rank);
      const double net_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
      move_copy(&pick, order[0]);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output, "%d,%d,net,0,%.3f,%.3f,0,0,0,-1,\"%s\"\n", root_id,
              PEG_BUDGET_ARM_NET, net_ms, net_ms, string_builder_peek(name));
    }
    static const int order_top_k[] = {PEG_BUDGET_ORDER_K, 8, 4, 2};
    static const int hybrid_top_k[] = {PEG_BUDGET_HYBRID_WIDTH, 16, 8, 4, 2};
    for (int bud = 0; bud < n_budgets; bud++) {
      for (int arm = 0; arm < 6; arm++) {
        if ((arm == 0 && !arm_peg) || (arm == 1 && !arm_peg_net) ||
            (arm == 2 && !arm_order) || (arm == 3 && !arm_hybrid) ||
            (arm == 4 && !arm_hybrid_utility) || (arm == 5 && !arm_peg_h2)) {
          continue;
        }
        double ms = 0, net_ms = 0, stage1_start_ms = -1;
        int stage = 0, stage1_done = 0;
        bool partial = false;
        peg1_net_chooser_reset(net);
        if (arm == 5) {
          // PEG asks the ranker itself; the budget covers its time.
          ranker.ms = 0;
          peg_budget_pick(root, threads, peg1_net_chooser_callback, net,
                          placements, n_placements, /*n_ranked=*/0,
                          /*top_k=*/NULL, /*n_top_k=*/0,
                          /*rank_by_utility=*/true, peg2_net_root_ranker,
                          &ranker, budgets[bud], &pick, &ms, &stage, &partial,
                          &stage1_done, &stage1_start_ms);
          net_ms = ranker.ms;
        } else if (arm >= 2) {
          const Move *order[PEG_STATIC_POOL_SIZE];
          int pick_rank;
          const int64_t start = ctimer_monotonic_ns();
          const int n_order = peg2_net_chooser_rank(
              chooser, root, candidates, order, PEG_BUDGET_ORDER_K, &pick_rank);
          // hybrid: the chooser's top K, then every other placement (hybridu:
          // only when the budget reaches PEG_BUDGET_HYBRID_GREEDY_MIN_MS).
          int n_only = n_order;
          for (int rank = 0; rank < n_order; rank++) {
            hybrid[rank] = order[rank];
          }
          const bool greedy_tail =
              arm == 3 || (arm == 4 && budgets[bud] * 1000 >=
                                           PEG_BUDGET_HYBRID_GREEDY_MIN_MS);
          if (greedy_tail) {
            for (int rank = 0; rank < n_placements; rank++) {
              bool in_order = false;
              for (int pos = 0; pos < n_order && !in_order; pos++) {
                in_order = compare_moves_without_equity(placements[rank],
                                                        order[pos], true) == -1;
              }
              if (!in_order) {
                hybrid[n_only++] = placements[rank];
              }
            }
          }
          net_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
          const double left = budgets[bud] - net_ms / 1e3;
          if (n_only < 2 || left <= 0) {
            move_copy(&pick, order[0]);
          } else {
            peg_budget_pick(
                root, threads, peg1_net_chooser_callback, net, hybrid, n_only,
                n_order, greedy_tail ? hybrid_top_k : order_top_k,
                greedy_tail ? (int)(sizeof(hybrid_top_k) / sizeof(int))
                            : (int)(sizeof(order_top_k) / sizeof(int)),
                arm == 4, /*ranker=*/NULL, /*ranker_ctx=*/NULL, left, &pick,
                &ms, &stage, &partial, &stage1_done, &stage1_start_ms);
            if (stage1_start_ms >= 0) {
              stage1_start_ms += net_ms;
            }
          }
          ms += net_ms;
        } else {
          peg_budget_pick(root, threads,
                          arm == 1 ? peg1_net_chooser_callback : NULL,
                          arm == 1 ? net : NULL, placements, n_placements,
                          /*n_ranked=*/0, /*top_k=*/NULL, /*n_top_k=*/0,
                          /*rank_by_utility=*/false, /*ranker=*/NULL,
                          /*ranker_ctx=*/NULL, budgets[bud], &pick, &ms, &stage,
                          &partial, &stage1_done, &stage1_start_ms);
        }
        static const char *const arm_names[] = {"peg",    "pegnet",  "order",
                                                "hybrid", "hybridu", "h2peg"};
        static const int arm_codes[] = {PEG_BUDGET_ARM_PEG,
                                        PEG_BUDGET_ARM_PEG_NET,
                                        PEG_BUDGET_ARM_NET_ORDER,
                                        PEG_BUDGET_ARM_HYBRID,
                                        PEG_BUDGET_ARM_HYBRID_UTILITY,
                                        PEG_BUDGET_ARM_PEG_H2};
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root), &pick,
                                game_get_ld(root), false);
        fprintf(output, "%d,%d,%s,%g,%.3f,%.3f,%d,%d,%d,%.3f,\"%s\"\n", root_id,
                arm_codes[arm] + bud, arm_names[arm], budgets[bud], ms, net_ms,
                stage, partial, stage1_done, stage1_start_ms,
                string_builder_peek(name));
      }
    }
    fflush(output);
  }
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  free(hybrid);
  free(placements);
  free(wide_moves);
  move_list_destroy(wide);
  move_list_destroy(ranker.pool);
  move_list_destroy(pool);
  peg2_net_chooser_destroy(chooser);
  peg1_net_chooser_destroy(net);
  config_destroy(config);
  free(done_roots);
  free(line);
}

void peg_budget_pick_ranks_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 8) {
    log_fatal("pegrank needs positions prefix, pegb csv, output, 2PEG model, "
              "2PEG first|-, stage N, top K");
  }
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  char **pick_text = malloc_or_die(sizeof(char *) * (size_t)pick_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *picks = fopen_or_die(string_splitter_get_item(fields, 2), "r");
  (void)getline_ignore_carriage_return(&line, &capacity, picks);
  while (getline_ignore_carriage_return(&line, &capacity, picks) > 0) {
    int root_id, code;
    char *open = strchr(line, '"'), *close = strrchr(line, '"');
    if (sscanf(line, "%d,%d,", &root_id, &code) != 2 || open == NULL ||
        close <= open) {
      log_fatal("pegrank invalid pick row");
    }
    *close = '\0';
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root =
          realloc_or_die(pick_root, sizeof(int) * (size_t)pick_capacity);
      pick_code =
          realloc_or_die(pick_code, sizeof(int) * (size_t)pick_capacity);
      pick_text =
          realloc_or_die(pick_text, sizeof(char *) * (size_t)pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks] = code;
    pick_text[num_picks] = string_duplicate(open + 1);
    num_picks++;
  }
  fclose(picks);
  FILE *output =
      fopen_exclusive_or_die(string_splitter_get_item(fields, 3), "w");
  fprintf(output, "root,code,net_rank,equity_rank\n");
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Peg2NetChooser *chooser = peg2_net_chooser_create(
      string_splitter_get_item(fields, 4), string_splitter_get_item(fields, 5),
      atoi(string_splitter_get_item(fields, 6)),
      atoi(string_splitter_get_item(fields, 7)), root, 1);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv",
                                    string_splitter_get_item(fields, 1));
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int pass_rank;
    const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("pegrank candidate replay mismatch");
    }
    const Move *order[PEG_STATIC_POOL_SIZE];
    int pick_rank;
    const int n_order = peg2_net_chooser_rank(chooser, root, candidates, order,
                                              PEG_STATIC_POOL_SIZE, &pick_rank);
    const int n_wide = peg_world_all_placements(root, wide, wide_moves);
    for (int pick_idx = 0; pick_idx < num_picks; pick_idx++) {
      if (pick_root[pick_idx] != root_id) {
        continue;
      }
      int net_rank = -1, equity_rank = -1;
      for (int pos = 0; pos < n_order && net_rank < 0; pos++) {
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root), order[pos],
                                game_get_ld(root), false);
        if (strings_equal(string_builder_peek(name), pick_text[pick_idx])) {
          net_rank = pos;
        }
      }
      for (int rank = 0; rank < n_wide && equity_rank < 0; rank++) {
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root), &wide_moves[rank],
                                game_get_ld(root), false);
        if (strings_equal(string_builder_peek(name), pick_text[pick_idx])) {
          equity_rank = rank;
        }
      }
      fprintf(output, "%d,%d,%d,%d\n", root_id, pick_code[pick_idx], net_rank,
              equity_rank);
    }
  }
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  free(wide_moves);
  move_list_destroy(wide);
  move_list_destroy(pool);
  peg2_net_chooser_destroy(chooser);
  config_destroy(config);
  for (int pick_idx = 0; pick_idx < num_picks; pick_idx++) {
    free(pick_text[pick_idx]);
  }
  free(pick_text);
  free(pick_code);
  free(pick_root);
  free(line);
}
