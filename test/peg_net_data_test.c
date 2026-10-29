#include "peg_net_data_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/value_net_history.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/value_net_features.h"
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

// peg1data input rows: the public scalars, then the mean, standard deviation,
// minimum and maximum of each extra feature over the hypothetical worlds.
enum {
  PEG1_LABEL_EXTRAS = 10,
  PEG1_LABEL_INPUTS = VALUE_NET_SCALARS + 4 * PEG1_LABEL_EXTRAS
};

static void peg1_label_extra_features(Game *after, int mover, MoveList *stuck,
                                      float *extra, double *cheap_ms,
                                      double *own_ms, double *opp_ms,
                                      bool compute_own, bool compute_opp) {
  const int64_t started = ctimer_monotonic_ns();
  memset(extra, 0, sizeof(float) * PEG1_LABEL_EXTRAS);
  const LetterDistribution *ld = game_get_ld(after);
  const Player *own = game_get_player(after, mover);
  const Player *opponent = game_get_player(after, 1 - mover);
  const Rack *own_rack = player_get_rack(own);
  const Rack *opp_rack = player_get_rack(opponent);
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const double points = equity_to_double(ld_get_score(ld, ml));
    extra[0] += rack_get_letter(own_rack, ml) * points;
    extra[1] += rack_get_letter(opp_rack, ml) * points;
  }
  extra[2] = rack_is_empty(own_rack) ? 0
                                     : equity_to_double(klv_get_leave_value(
                                           player_get_klv(own), own_rack));
  extra[3] = rack_is_empty(opp_rack) ? 0
                                     : equity_to_double(klv_get_leave_value(
                                           player_get_klv(opponent), opp_rack));
  extra[4] = rack_get_total_letters(own_rack);
  extra[5] = rack_get_total_letters(opp_rack);
  *cheap_ms = (double)(ctimer_monotonic_ns() - started) / 1e6;
  const int64_t own_start = ctimer_monotonic_ns();
  if (compute_own && !game_over(after)) {
    endgame_stuck_fractions(after, stuck, mover, &extra[6], &extra[8]);
  }
  *own_ms = (double)(ctimer_monotonic_ns() - own_start) / 1e6;
  const int64_t opp_start = ctimer_monotonic_ns();
  if (compute_opp && !game_over(after)) {
    endgame_stuck_fractions(after, stuck, 1 - mover, &extra[7], &extra[9]);
  }
  *opp_ms = (double)(ctimer_monotonic_ns() - opp_start) / 1e6;
}

static double peg1_endgame_label(Game *after, int mover, EndgameCtx **ctx,
                                 EndgameResults *results,
                                 ThreadControl *control) {
  double margin = spread_for_player(after, mover);
  if (!game_over(after)) {
    const EndgameArgs args = {.game = after,
                              .thread_control = control,
                              .plies = 2,
                              .tt_fraction_of_mem = 0,
                              .initial_small_move_arena_size =
                                  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                              .num_threads = 1,
                              .use_heuristics = true,
                              .num_top_moves = 1,
                              .incremental_movegen = true,
                              .enable_pv_display = true,
                              .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT};
    endgame_solve_inline(ctx, &args, results);
    if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED ||
        endgame_results_get_depth(results, ENDGAME_RESULT_BEST) != 2) {
      log_fatal("peg1 incomplete depth2 target");
    }
    margin -= endgame_results_get_value(results, ENDGAME_RESULT_BEST);
  }
  return margin;
}

void peg1_labels_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1data needs count, offset, seed and prefix");
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg1 invalid count or offset");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scenario = game_duplicate(game);
  Game *hidden_swap = game_duplicate(game);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *stuck = move_list_create(1);
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  const char *suffixes[] = {"positions.csv", "labels.csv", "scenarios.csv",
                            "inputs.f32"};
  FILE *files[4];
  for (int idx = 0; idx < 4; idx++) {
    char *path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    files[idx] = fopen_exclusive_or_die(path, "wb");
    free(path);
  }
  fprintf(files[0], "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                    "rows,pass_rank,movegen_ms,cgp\n");
  fprintf(files[1], "row,root,candidate,candidates,after,static_equity,win,"
                    "margin,utility,scenario_count,feature_ms,cheap_ms,own_"
                    "stuck_ms,opp_stuck_ms,solve_ms,move\n");
  fprintf(files[2], "row,root,candidate,tile,multiplicity,depth,win,margin,own_"
                    "score,opp_score,own_klv,opp_klv,own_count,opp_count,own_"
                    "stuck,opp_stuck,own_stuck_points,opp_stuck_points\n");
  StringBuilder *name = string_builder_create();
  uint64_t row = 0;
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    ValueNetHistory histories[2];
    int source_turns = 0;
    uint64_t source_seed = 0;
    int attempt = 0;
    bool ready = false;
    Move candidates[PEG_STATIC_POOL_SIZE];
    int count = 0;
    int pass_rank = -1;
    double movegen_ms = 0;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg1 source generation failed");
      }
      source_seed =
          splitmix64(seed ^ splitmix64((uint64_t)root_id) ^
                     splitmix64((uint64_t)attempt + UINT64_C(748238)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      value_net_history_reset(&histories[0]);
      value_net_history_reset(&histories[1]);
      source_turns = 0;
      while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
             source_turns < PEG_NET_MAX_TURNS) {
        Move move;
        move_copy(&move, top_equity_move(game, static_list));
        const int mover = game_get_player_on_turn_index(game);
        value_net_history_record_opponent_move(&histories[1 - mover], &move);
        play_move(&move, game, NULL);
        source_turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == 1) {
        const int64_t start = ctimer_monotonic_ns();
        count = peg_world_static_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        ready = count > 0;
      }
    }
    const int mover = game_get_player_on_turn_index(game);
    int unseen[VALUE_NET_TILE_TYPES] = {0};
    int total = 0;
    const Rack *opponent = player_get_rack(game_get_player(game, 1 - mover));
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      unseen[ml] = rack_get_letter(opponent, ml) +
                   bag_get_letter(game_get_bag(game), (MachineLetter)ml);
      total += unseen[ml];
    }
    if (total != 8) {
      log_fatal("peg1 source does not have eight unseen tiles");
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(files[0], "%d,%llu,%d,%d,%d,1,%llu,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, source_turns, mover,
            (unsigned long long)row, count, pass_rank, movegen_ms, cgp);
    free(cgp);
    // Permute the PRIVATE split and require exactly identical public scalar
    // inputs and candidate ordering. All extra inputs below depend only on
    // those public counts and enumerated hypothetical worlds.
    int swapped_tile = 0;
    while (!unseen[swapped_tile]) {
      swapped_tile++;
    }
    peg_world_from_public_counts(game, unseen, swapped_tile, hidden_swap);
    Move permuted[PEG_STATIC_POOL_SIZE];
    int swapped_pass;
    if (peg_world_static_pool(hidden_swap, pool, permuted, &swapped_pass) !=
            count ||
        swapped_pass != pass_rank) {
      log_fatal("peg1 private partition changes candidate pool");
    }
    for (int candidate = 0; candidate < count; candidate++, row++) {
      const Move *move = &candidates[candidate];
      if (compare_moves(move, &permuted[candidate], true) != -1) {
        log_fatal("peg1 private partition changes move order");
      }
      float inputs[PEG1_LABEL_INPUTS] = {0};
      float swapped[VALUE_NET_SCALARS];
      value_net_scalars_for_move(game, move, &histories[mover], inputs);
      value_net_scalars_for_move(hidden_swap, move, &histories[mover], swapped);
      if (memcmp(inputs, swapped, sizeof(swapped)) != 0) {
        log_fatal("peg1 private partition changes public scalars");
      }
      double sums[PEG1_LABEL_EXTRAS] = {0};
      double squared[PEG1_LABEL_EXTRAS] = {0};
      float low[PEG1_LABEL_EXTRAS], high[PEG1_LABEL_EXTRAS];
      for (int idx = 0; idx < PEG1_LABEL_EXTRAS; idx++) {
        low[idx] = INFINITY;
        high[idx] = -INFINITY;
      }
      double win_sum = 0, margin_sum = 0, utility_sum = 0;
      double feature_ms = 0, cheap_ms = 0, own_ms = 0, opp_ms = 0, solve_ms = 0;
      int scenarios = 0;
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (!unseen[tile]) {
          continue;
        }
        scenarios++;
        const int64_t feature_start = ctimer_monotonic_ns();
        peg_world_from_public_counts(game, unseen, tile, scenario);
        play_move(move, scenario, NULL);
        if (bag_get_letters(game_get_bag(scenario)) != 0 ||
            game_over(scenario)) {
          log_fatal("peg1 placement did not produce a nonterminal empty bag");
        }
        float extras[PEG1_LABEL_EXTRAS];
        double cheap, own, opp;
        peg1_label_extra_features(scenario, mover, stuck, extras, &cheap, &own,
                                  &opp, true, true);
        feature_ms += (double)(ctimer_monotonic_ns() - feature_start) / 1e6;
        cheap_ms += cheap;
        own_ms += own;
        opp_ms += opp;
        const double probability = (double)unseen[tile] / total;
        for (int idx = 0; idx < PEG1_LABEL_EXTRAS; idx++) {
          sums[idx] += probability * extras[idx];
          squared[idx] += probability * extras[idx] * extras[idx];
          low[idx] = fminf(low[idx], extras[idx]);
          high[idx] = fmaxf(high[idx], extras[idx]);
        }
        const int64_t solve_start = ctimer_monotonic_ns();
        const double margin =
            peg1_endgame_label(scenario, mover, &ctx, results, control);
        solve_ms += (double)(ctimer_monotonic_ns() - solve_start) / 1e6;
        const double win = margin > 0 ? 1 : margin < 0 ? 0 : .5;
        const double utility = (win + .5 / (1 + exp(-margin / 100))) / 1.5;
        win_sum += probability * win;
        margin_sum += probability * margin;
        utility_sum += probability * utility;
        fprintf(files[2], "%llu,%d,%d,%d,%d,2,%.1f,%.9g",
                (unsigned long long)row, root_id, candidate, tile, unseen[tile],
                win, margin);
        for (int idx = 0; idx < PEG1_LABEL_EXTRAS; idx++) {
          fprintf(files[2], ",%.9g", extras[idx]);
        }
        fprintf(files[2], "\n");
      }
      for (int idx = 0; idx < PEG1_LABEL_EXTRAS; idx++) {
        inputs[VALUE_NET_SCALARS + idx] = sums[idx];
        inputs[VALUE_NET_SCALARS + PEG1_LABEL_EXTRAS + idx] =
            sqrt(fmax(0, squared[idx] - sums[idx] * sums[idx]));
        inputs[VALUE_NET_SCALARS + 2 * PEG1_LABEL_EXTRAS + idx] = low[idx];
        inputs[VALUE_NET_SCALARS + 3 * PEG1_LABEL_EXTRAS + idx] = high[idx];
      }
      if (!fwrite_le_floats(inputs, PEG1_LABEL_INPUTS, files[3])) {
        log_fatal("peg1 input write failed");
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(game), move,
                              game_get_ld(game), false);
      fprintf(files[1],
              "%llu,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%.9g,%."
              "9g,\"%s\"\n",
              (unsigned long long)row, root_id, candidate, count,
              value_net_spread_after_move(game, move),
              equity_to_double(move_get_equity(move)), win_sum, margin_sum,
              utility_sum, scenarios, feature_ms, cheap_ms, own_ms, opp_ms,
              solve_ms, string_builder_peek(name));
    }
    for (int idx = 0; idx < 4; idx++) {
      fflush(files[idx]);
    }
    printf("peg1 root %d complete candidates %d\n", root_id, count);
    fflush(stdout);
  }
  for (int idx = 0; idx < 4; idx++) {
    fclose(files[idx]);
  }
  string_builder_destroy(name);
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  move_list_destroy(stuck);
  game_destroy(scenario);
  game_destroy(hidden_swap);
  config_destroy(config);
}

void peg1_roots_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1roots needs count, offset, seed and prefix");
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg1roots invalid count or offset");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scenario = game_duplicate(game), *work = game_duplicate(game);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *small = move_list_create_small(16);
  Move *move = move_create();
  const char *suffixes[] = {"positions.csv", "candidates.csv", "scalars.f32",
                            "reply.f32", "reply-timing.csv"};
  FILE *files[5];
  for (int idx = 0; idx < 5; idx++) {
    char *path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    files[idx] = fopen_exclusive_or_die(path, "wb");
    free(path);
  }
  fprintf(files[0], "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                    "rows,pass_rank,movegen_ms,cgp\n");
  fprintf(files[1], "row,root,candidate,candidates,after,static_equity,tiles,"
                    "move\n");
  fprintf(files[4], "root,candidates,worlds,movegen_ms,world_ms,mover_now_ms,"
                    "opp_reply_ms,playout_ms\n");
  StringBuilder *name = string_builder_create();
  uint64_t row = 0;
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    ValueNetHistory histories[2];
    int source_turns = 0;
    uint64_t source_seed = 0;
    int attempt = 0;
    bool ready = false;
    Move candidates[PEG_STATIC_POOL_SIZE];
    int count = 0;
    int pass_rank = -1;
    double movegen_ms = 0;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg1roots source generation failed");
      }
      source_seed =
          splitmix64(seed ^ splitmix64((uint64_t)root_id) ^
                     splitmix64((uint64_t)attempt + UINT64_C(748238)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      value_net_history_reset(&histories[0]);
      value_net_history_reset(&histories[1]);
      source_turns = 0;
      while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
             source_turns < PEG_NET_MAX_TURNS) {
        Move played;
        move_copy(&played, top_equity_move(game, static_list));
        const int mover = game_get_player_on_turn_index(game);
        value_net_history_record_opponent_move(&histories[1 - mover], &played);
        play_move(&played, game, NULL);
        source_turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == 1) {
        const int64_t start = ctimer_monotonic_ns();
        count = peg_world_static_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        ready = count > 0;
      }
    }
    const int mover = game_get_player_on_turn_index(game);
    int unseen[VALUE_NET_TILE_TYPES] = {0};
    int total = 0, worlds = 0;
    const Rack *opponent = player_get_rack(game_get_player(game, 1 - mover));
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      unseen[ml] = rack_get_letter(opponent, ml) +
                   bag_get_letter(game_get_bag(game), (MachineLetter)ml);
      total += unseen[ml];
      worlds += unseen[ml] > 0;
    }
    if (total != 8) {
      log_fatal("peg1roots source does not have eight unseen tiles");
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(files[0], "%d,%llu,%d,%d,%d,1,%llu,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, source_turns, mover,
            (unsigned long long)row, count, pass_rank, movegen_ms, cgp);
    free(cgp);
    double timing[4] = {0};
    for (int candidate = 0; candidate < count; candidate++, row++) {
      const Move *cand = &candidates[candidate];
      float scalars[VALUE_NET_SCALARS];
      value_net_scalars_for_move(game, cand, &histories[mover], scalars);
      if (!fwrite_le_floats(scalars, VALUE_NET_SCALARS, files[2])) {
        log_fatal("peg1roots scalar write failed");
      }
      float reply[PEG1_REPLY_ROW] = {0};
      reply[0] = (float)value_net_spread_after_move(game, cand);
      reply[1] = (float)equity_to_double(move_get_equity(cand));
      reply[2] = (float)move_get_tiles_played(cand);
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (unseen[tile] == 0) {
          continue;
        }
        const int64_t world_start = ctimer_monotonic_ns();
        peg_world_from_public_counts(game, unseen, tile, scenario);
        play_move(cand, scenario, NULL);
        if (bag_get_letters(game_get_bag(scenario)) != 0 ||
            game_over(scenario)) {
          log_fatal("peg1roots placement did not empty the bag");
        }
        timing[0] += (double)(ctimer_monotonic_ns() - world_start) / 1e6;
        peg1_reply_features(
            scenario, mover, work, small, move,
            &reply[PEG1_REPLY_ROW_HEAD + tile * PEG1_REPLY_FEATURES], timing);
      }
      if (!fwrite_le_floats(reply, PEG1_REPLY_ROW, files[3])) {
        log_fatal("peg1roots reply write failed");
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(game), cand,
                              game_get_ld(game), false);
      fprintf(files[1], "%llu,%d,%d,%d,%.9g,%.9g,%d,\"%s\"\n",
              (unsigned long long)row, root_id, candidate, count, reply[0],
              reply[1], move_get_tiles_played(cand), string_builder_peek(name));
    }
    fprintf(files[4], "%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g\n", root_id, count,
            worlds, movegen_ms, timing[0], timing[1], timing[2], timing[3]);
    for (int idx = 0; idx < 5; idx++) {
      fflush(files[idx]);
    }
  }
  for (int idx = 0; idx < 5; idx++) {
    fclose(files[idx]);
  }
  string_builder_destroy(name);
  move_destroy(move);
  move_list_destroy(small);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  game_destroy(scenario);
  game_destroy(work);
  config_destroy(config);
}

void peg1_features_v2_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 3) {
    log_fatal("peg1feat2 needs data prefix and output prefix");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Peg1Scratch work = peg1_scratch_create(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.reply2.f32", prefix);
  FILE *output = fopen_exclusive_or_die(path, "wb");
  free(path);
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
      log_fatal("peg1feat2 invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1feat2 invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1feat2 CGP load failed");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int current_pass;
    const int count =
        peg_world_static_pool(root, pool, candidates, &current_pass);
    if (count != expected || current_pass != pass_rank) {
      log_fatal("peg1feat2 candidate replay mismatch");
    }
    Peg1RootContext prep;
    peg1_root_context_fill(root, &work, &prep, true);
    Peg1NetTiming timing = {0};
    for (int candidate = 0; candidate < count; candidate++) {
      const Move *cand = &candidates[candidate];
      const uint64_t touched =
          peg_world_lane_influence(game_get_board(root), cand);
      float row[PEG1_REPLY_ROW] = {0};
      row[0] = (float)value_net_spread_after_move(root, cand);
      row[1] = (float)equity_to_double(move_get_equity(cand));
      row[2] = (float)move_get_tiles_played(cand);
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (prep.unseen[tile] == 0) {
          continue;
        }
        peg1_reply_features_v2(
            root, cand, touched, tile, &prep, &work, true, true,
            &row[PEG1_REPLY_ROW_HEAD + tile * PEG1_REPLY_FEATURES], &timing);
      }
      if (!fwrite_le_floats(row, PEG1_REPLY_ROW, output)) {
        log_fatal("peg1feat2 write failed");
      }
    }
    fflush(output);
  }
  free(line);
  fclose(positions);
  fclose(output);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  peg1_scratch_destroy(&work);
  config_destroy(config);
}

void peg1_positions_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1pos needs count, offset, seed and prefix");
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg1pos invalid count or offset");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.positions.csv", prefix);
  FILE *output = fopen_exclusive_or_die(path, "w");
  free(path);
  fprintf(output, "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                  "rows,pass_rank,movegen_ms,cgp\n");
  uint64_t row = 0;
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    uint64_t source_seed = 0;
    int attempt = 0, source_turns = 0, count = 0, pass_rank = -1;
    bool ready = false;
    Move candidates[PEG_STATIC_POOL_SIZE];
    double movegen_ms = 0;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg1pos source generation failed");
      }
      source_seed =
          splitmix64(seed ^ splitmix64((uint64_t)root_id) ^
                     splitmix64((uint64_t)attempt + UINT64_C(748238)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      source_turns = 0;
      while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
             source_turns < PEG_NET_MAX_TURNS) {
        Move played;
        move_copy(&played, top_equity_move(game, static_list));
        play_move(&played, game, NULL);
        source_turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == 1) {
        const int64_t start = ctimer_monotonic_ns();
        count = peg_world_static_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        ready = count > 0;
      }
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(output, "%d,%llu,%d,%d,%d,1,%llu,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, source_turns,
            game_get_player_on_turn_index(game), (unsigned long long)row, count,
            pass_rank, movegen_ms, cgp);
    free(cgp);
    row += count;
    fflush(output);
  }
  fclose(output);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  config_destroy(config);
}

void peg1_deep_labels_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 6) {
    log_fatal("peg1deep needs positions prefix, choices CSV, output prefix, "
              "plies, tt fraction");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 5), NULL);
  if (plies < 1 || plies > 25 || tt_fraction < 0 || tt_fraction > 0.1) {
    log_fatal("peg1deep invalid plies or tt fraction");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *scenario = game_duplicate(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  ErrorStack *errors = error_stack_create();
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  FILE *choices = fopen_or_die(choices_path, "r");
  path = get_formatted_string("%s.deep.csv", prefix);
  FILE *output = fopen_exclusive_or_die(path, "w");
  free(path);
  fprintf(output, "root,candidate,tile,multiplicity,plies,margin,solve_ms\n");
  char *line = NULL, *choice_line = NULL;
  size_t capacity = 0, choice_capacity = 0;
  int pending_root = -1, pending_candidate = -1;
  bool have_pending = false;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1deep invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1deep invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1deep CGP load failed");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int current_pass;
    const int count =
        peg_world_static_pool(root, pool, candidates, &current_pass);
    if (count != expected) {
      log_fatal("peg1deep candidate replay mismatch");
    }
    int unseen[VALUE_NET_TILE_TYPES];
    peg_world_public_unseen(root, unseen);
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
          log_fatal("peg1deep choices out of order");
        }
        break;
      }
      have_pending = false;
      if (pending_candidate < 0 || pending_candidate >= count) {
        log_fatal("peg1deep invalid candidate");
      }
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (unseen[tile] == 0) {
          continue;
        }
        peg_world_from_public_counts(root, unseen, tile, scenario);
        play_move(&candidates[pending_candidate], scenario, NULL);
        const int mover = seat;
        double margin = spread_for_player(scenario, mover);
        int depth_done = 0;
        const int64_t start = ctimer_monotonic_ns();
        if (!game_over(scenario)) {
          const EndgameArgs args = {.game = scenario,
                                    .thread_control = control,
                                    .plies = plies,
                                    .tt_fraction_of_mem = tt_fraction,
                                    .initial_small_move_arena_size =
                                        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                                    .num_threads = 1,
                                    .use_heuristics = true,
                                    .num_top_moves = 1,
                                    .incremental_movegen = true,
                                    .enable_pv_display = true,
                                    .dual_lexicon_mode =
                                        DUAL_LEXICON_MODE_IGNORANT};
          endgame_solve_inline(&ctx, &args, results);
          if (endgame_results_get_status(results) !=
              ENDGAME_RESULT_STATUS_FINISHED) {
            log_fatal("peg1deep search did not finish");
          }
          margin -= endgame_results_get_value(results, ENDGAME_RESULT_BEST);
          depth_done = endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
        }
        fprintf(output, "%d,%d,%d,%d,%d,%.9g,%.9g\n", root_id,
                pending_candidate, tile, unseen[tile], depth_done, margin,
                (double)(ctimer_monotonic_ns() - start) / 1e6);
      }
      fflush(output);
    }
  }
  free(line);
  free(choice_line);
  fclose(positions);
  fclose(choices);
  fclose(output);
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  game_destroy(scenario);
  config_destroy(config);
}

// Fresh PEG boards from static self-play: the first turn with bag_size tiles
// in the bag (both racks full). Fields: count, offset, seed, output prefix.
static void peg_positions_generate(const StringSplitter *fields, int bag_size) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg%dpos needs count, offset, seed and prefix", bag_size);
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg2pos invalid count or offset");
  }
  Config *config = peg_net_config_create();
  Game *game = config_get_game(config);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.positions.csv", prefix);
  FILE *output = fopen_exclusive_or_die(path, "w");
  free(path);
  fprintf(output, "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                  "rows,pass_rank,movegen_ms,cgp\n");
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    uint64_t source_seed = 0;
    int attempt = 0, turns = 0, count = 0, pass_rank = -1;
    double movegen_ms = 0;
    bool ready = false;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg2 source generation failed");
      }
      source_seed =
          splitmix64(seed ^ splitmix64((uint64_t)root_id) ^
                     splitmix64((uint64_t)attempt + UINT64_C(748239)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      turns = 0;
      while (!game_over(game) &&
             bag_get_letters(game_get_bag(game)) > bag_size &&
             turns < PEG_NET_MAX_TURNS) {
        Move move;
        move_copy(&move, top_equity_move(game, static_list));
        play_move(&move, game, NULL);
        turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == bag_size) {
        Move candidates[PEG_STATIC_POOL_SIZE];
        const int64_t start = ctimer_monotonic_ns();
        count = peg_world_static_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        int unseen[VALUE_NET_TILE_TYPES];
        ready = count > 0 &&
                peg_world_public_unseen(game, unseen) == RACK_SIZE + bag_size;
      }
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(output, "%d,%llu,%d,%d,%d,%d,0,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, turns,
            game_get_player_on_turn_index(game), bag_size, count, pass_rank,
            movegen_ms, cgp);
    free(cgp);
    fflush(output);
  }
  fclose(output);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  config_destroy(config);
}

void peg2_positions_mode(const StringSplitter *fields) {
  peg_positions_generate(fields, 2);
}

void peg_positions_mode(const StringSplitter *fields) {
  const char *bag_env = getenv("PEG_POSITIONS_BAG");
  const int bag_size = bag_env != NULL ? atoi(bag_env) : 0;
  if (bag_size < 1 || bag_size > RACK_SIZE) {
    log_fatal("pegnpos needs PEG_POSITIONS_BAG in 1..%d", RACK_SIZE);
  }
  peg_positions_generate(fields, bag_size);
}

// Optional candidate lists (env PEG2_CANDIDATE_LIST: a CSV of root,wide rank
// rows). peg2data and peg2feat then score exactly the listed ranks of each
// board, in file order, instead of the static top K.
typedef struct Peg2CandidateList {
  int count;
  int *root;
  int *rank;
} Peg2CandidateList;

static Peg2CandidateList peg2_candidate_list_load(void) {
  Peg2CandidateList ranks = {.count = -1, .root = NULL, .rank = NULL};
  const char *path = getenv("PEG2_CANDIDATE_LIST");
  if (path == NULL || path[0] == '\0') {
    return ranks;
  }
  FILE *file = fopen_or_die(path, "r");
  int capacity = 1024;
  ranks.count = 0;
  ranks.root = malloc_or_die(sizeof(int) * capacity);
  ranks.rank = malloc_or_die(sizeof(int) * capacity);
  char *line = NULL;
  size_t line_capacity = 0;
  while (getline_ignore_carriage_return(&line, &line_capacity, file) >= 0) {
    int root_id, rank;
    if (sscanf(line, "%d,%d", &root_id, &rank) != 2) {
      continue;
    }
    if (rank < 0) {
      log_fatal("peg2 rank list has a negative rank");
    }
    if (ranks.count == capacity) {
      capacity *= 2;
      ranks.root = realloc_or_die(ranks.root, sizeof(int) * capacity);
      ranks.rank = realloc_or_die(ranks.rank, sizeof(int) * capacity);
    }
    ranks.root[ranks.count] = root_id;
    ranks.rank[ranks.count++] = rank;
  }
  free(line);
  fclose(file);
  return ranks;
}

static void peg2_candidate_list_destroy(Peg2CandidateList *ranks) {
  free(ranks->root);
  free(ranks->rank);
}

// The candidates one board scores: the static top K, or (with a rank list)
// the listed wide ranks. Returns how many; chosen[slot] is each one's move
// and ids[slot] its candidate id. *wide_count is the length of the list the
// ids index (the static pool, or the wide list).
static int peg2_scored_candidates(const Game *root, int root_id,
                                  const Peg2CandidateList *ranks,
                                  const Move *pool_moves, int count, int top_k,
                                  MoveList *wide, Move *wide_moves,
                                  int *wide_count, const Move **chosen,
                                  int *ids) {
  if (ranks->count < 0) {
    const int scored = count < top_k ? count : top_k;
    for (int slot = 0; slot < scored; slot++) {
      chosen[slot] = &pool_moves[slot];
      ids[slot] = slot;
    }
    *wide_count = count;
    return scored;
  }
  *wide_count = peg_world_all_placements(root, wide, wide_moves);
  for (int rank = 0; rank < count; rank++) {
    if (compare_moves(&wide_moves[rank], &pool_moves[rank], true) != -1) {
      log_fatal("peg2 wide ranks disagree with the static pool");
    }
  }
  int scored = 0;
  for (int idx = 0; idx < ranks->count; idx++) {
    if (ranks->root[idx] != root_id) {
      continue;
    }
    const int rank = ranks->rank[idx];
    if (rank >= *wide_count || scored == PEG_WORLD_ALL_PLACEMENTS_CAP) {
      log_fatal("peg2 rank past the move list");
    }
    chosen[scored] = &wide_moves[rank];
    ids[scored++] = rank;
  }
  return scored;
}

void peg2_labels_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 11) {
    log_fatal("peg2data needs positions prefix, output prefix, top K, plies, "
              "first, second|-, shortlist, w_spread, scale, check boards");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const int top_k = atoi(string_splitter_get_item(fields, 3));
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const int check_boards = atoi(string_splitter_get_item(fields, 10));
  if (top_k < 1 || top_k > PEG_STATIC_POOL_SIZE || plies < 1 || plies > 25) {
    log_fatal("peg2data invalid top K or plies");
  }
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Game *world = game_duplicate(root);
  Peg1NetChooser *net = peg1_net_chooser_create(
      string_splitter_get_item(fields, 5), string_splitter_get_item(fields, 6),
      atoi(string_splitter_get_item(fields, 7)),
      strtod(string_splitter_get_item(fields, 8), NULL),
      strtod(string_splitter_get_item(fields, 9), NULL), root, 1);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *static_list = move_list_create(1);
  Peg2CandidateList ranks = peg2_candidate_list_load();
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  const Move **chosen =
      malloc_or_die(sizeof(Move *) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  int *ids = malloc_or_die(sizeof(int) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  // Resume: boards already in the labels file are skipped.
  char *labels_path = get_formatted_string("%s.labels.csv", prefix);
  char *scenarios_path = get_formatted_string("%s.scenarios.csv", prefix);
  char *line = NULL;
  size_t capacity = 0;
  int done_max = -1;
  bool fresh = true;
  FILE *previous = fopen(labels_path, "r");
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id;
      if (sscanf(line, "%d,", &root_id) == 1 && root_id > done_max) {
        done_max = root_id;
      }
    }
    fclose(previous);
  }
  FILE *labels = fopen_or_die(labels_path, "a");
  FILE *scenarios = fopen_or_die(scenarios_path, "a");
  free(labels_path);
  free(scenarios_path);
  if (fresh) {
    fprintf(labels, "root,candidate,candidates,tiles,after,static_equity,win,"
                    "margin,worlds,net_calls,net_ms,solve_ms,move\n");
    fprintf(scenarios, "root,candidate,bag_a,bag_b,drawn,multiplicity,"
                       "reply_pass,reply_tiles,reply_score,turns,win,"
                       "margin\n");
  }
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
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
    net->check = boards++ < check_boards;
    // Rows are written in board order, so a resumed run continues after the
    // last board in the labels file (that board's rows were flushed whole).
    if (root_id <= done_max) {
      continue;
    }
    const int mover = game_get_player_on_turn_index(root);
    int unseen[VALUE_NET_TILE_TYPES];
    if (peg_world_public_unseen(root, unseen) != PEG2_UNSEEN) {
      log_fatal("peg2 root does not have nine unseen tiles");
    }
    Move candidates[PEG_STATIC_POOL_SIZE];
    int pass_rank;
    const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg2 candidate replay mismatch");
    }
    int wide_count;
    const int scored =
        peg2_scored_candidates(root, root_id, &ranks, candidates, count, top_k,
                               wide, wide_moves, &wide_count, chosen, ids);
    for (int slot = 0; slot < scored; slot++) {
      const int candidate = ids[slot];
      const Move *cand = chosen[slot];
      const int tiles = move_get_tiles_played(cand);
      const int n_drawn = tiles < 2 ? tiles : 2;
      int64_t calls_before, checks;
      double net_ms_before;
      peg1_net_chooser_totals(net, &calls_before, &checks, &net_ms_before);
      double win_sum = 0, margin_sum = 0, weight_sum = 0, solve_ms = 0;
      int worlds = 0;
      for (int tile_a = 0; tile_a < VALUE_NET_TILE_TYPES; tile_a++) {
        for (int tile_b = tile_a; tile_b < VALUE_NET_TILE_TYPES; tile_b++) {
          const int pairs = tile_a == tile_b
                                ? unseen[tile_a] * (unseen[tile_a] - 1) / 2
                                : unseen[tile_a] * unseen[tile_b];
          if (pairs == 0) {
            continue;
          }
          // The opponent's reply depends only on its rack, i.e. on the
          // unordered pair, so one network pick serves both draw orders.
          bool have_reply = false;
          Move reply;
          const int orders = n_drawn == 1 && tile_a != tile_b ? 2 : 1;
          for (int order = 0; order < orders; order++) {
            const MachineLetter bag2[2] = {
                (MachineLetter)(order == 0 ? tile_a : tile_b),
                (MachineLetter)(order == 0 ? tile_b : tile_a)};
            // Weight in ordered draws of the two bag tiles.
            const double weight = n_drawn == 1
                                      ? (tile_a == tile_b ? 2.0 * pairs : pairs)
                                      : 2.0 * pairs;
            peg_world_after_candidate(root, unseen, cand, bag2, n_drawn, world);
            int reply_pass = -1, reply_tiles = -1, turns = 0;
            double reply_score = 0, ignored_ms = 0;
            if (n_drawn == 1) {
              if (!have_reply) {
                if (!peg1_net_chooser_pick(net, 0, world, &reply)) {
                  log_fatal("peg2 opponent reply is not a 1PEG");
                }
                have_reply = true;
              }
              reply_pass = move_get_type(&reply) == GAME_EVENT_PASS;
              reply_tiles = move_get_tiles_played(&reply);
              reply_score = equity_to_double(move_get_score(&reply));
              play_move(&reply, world, NULL);
              turns = 1 + peg2_play_drawing_turns(world, net, PEG2_REPLY_NET,
                                                  static_list, &ignored_ms);
            }
            const int64_t start = ctimer_monotonic_ns();
            const double margin = peg2_fixed_depth_label(
                world, mover, plies, &ctx, results, control);
            solve_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
            const double win = margin > 0 ? 1 : margin < 0 ? 0 : .5;
            win_sum += weight * win;
            margin_sum += weight * margin;
            weight_sum += weight;
            worlds++;
            fprintf(scenarios, "%d,%d,%d,%d,%d,%.0f,%d,%d,%.9g,%d,%.1f,%.9g\n",
                    root_id, candidate, bag2[0], bag2[1], n_drawn, weight,
                    reply_pass, reply_tiles, reply_score, turns, win, margin);
          }
        }
      }
      if (weight_sum != PEG2_UNSEEN * (PEG2_UNSEEN - 1)) {
        log_fatal("peg2 world weights do not cover the unseen pool");
      }
      int64_t calls_after;
      double net_ms_after;
      peg1_net_chooser_totals(net, &calls_after, &checks, &net_ms_after);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), cand,
                              game_get_ld(root), false);
      fprintf(
          labels, "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%d,%lld,%.9g,%.9g,\"%s\"\n",
          root_id, candidate, wide_count, tiles,
          value_net_spread_after_move(root, cand),
          equity_to_double(move_get_equity(cand)), win_sum / weight_sum,
          margin_sum / weight_sum, worlds,
          (long long)(calls_after - calls_before), net_ms_after - net_ms_before,
          solve_ms, string_builder_peek(name));
    }
    fflush(scenarios);
    fsync(fileno(scenarios));
    fflush(labels);
    fsync(fileno(labels));
    int64_t calls, checks;
    double net_ms;
    peg1_net_chooser_totals(net, &calls, &checks, &net_ms);
    printf("peg2 root %d candidates %d net calls %lld checks %lld\n", root_id,
           scored, (long long)calls, (long long)checks);
    fflush(stdout);
  }
  free(line);
  fclose(positions);
  fclose(labels);
  fclose(scenarios);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  peg2_candidate_list_destroy(&ranks);
  move_list_destroy(wide);
  free(wide_moves);
  free(chosen);
  free(ids);
  peg1_net_chooser_destroy(net);
  game_destroy(world);
  config_destroy(config);
}

void peg2_features_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 11) {
    log_fatal("peg2feat needs positions prefix, output prefix, top K, first, "
              "second|-, shortlist, w_spread, scale, net features, check "
              "boards");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const int top_k = atoi(string_splitter_get_item(fields, 3));
  const bool net_features = atoi(string_splitter_get_item(fields, 9)) != 0;
  const int check_boards = atoi(string_splitter_get_item(fields, 10));
  if (top_k < 1 || top_k > PEG_STATIC_POOL_SIZE) {
    log_fatal("peg2feat invalid top K");
  }
  Config *config = peg_net_config_create();
  Game *root = config_get_game(config);
  Game *world = game_duplicate(root);
  Game *swapped_root = game_duplicate(root);
  Peg1NetChooser *net = peg1_net_chooser_create(
      string_splitter_get_item(fields, 4), string_splitter_get_item(fields, 5),
      atoi(string_splitter_get_item(fields, 6)),
      strtod(string_splitter_get_item(fields, 7), NULL),
      strtod(string_splitter_get_item(fields, 8), NULL), root, 1);
  Peg1Scratch work = peg1_scratch_create(root);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  Peg2CandidateList ranks = peg2_candidate_list_load();
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  const Move **chosen =
      malloc_or_die(sizeof(Move *) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  int *ids = malloc_or_die(sizeof(int) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.cands.csv", prefix);
  FILE *cands = fopen_exclusive_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.public.f32", prefix);
  FILE *public_out = fopen_exclusive_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.worlds.f32", prefix);
  FILE *worlds_out = fopen_exclusive_or_die(path, "w");
  free(path);
  // worlds.f32 columns: 0 weight (ordered-draw probability), 1 opponent
  // top-scoring reply (+ mover rack x2 when it goes out), 2 out, 3 tiles,
  // 4 pass, 5 mover greedy follow-up (+ opponent rack x2 when out), 6 its out
  // flag, 7 network features present, 8 network win, 9 network spread,
  // 10 network utility, 11 network pick score, 12 its tiles, 13 its pass
  // flag, 14 tile drawn by the mover (one-tile plays; -1 otherwise), 15 tile
  // left in the bag (one-tile plays; -1 otherwise).
  fprintf(cands, "root,candidate,candidates,tiles,after,static_equity,"
                 "world_rows,prep_ms,cheap_ms,net_ms,net_calls\n");
  path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  ValueNetHistory history;
  value_net_history_reset(&history);
  int boards = 0;
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = peg_positions_next_root(
        positions, &line, &capacity, root, errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    const bool check = boards++ < check_boards;
    const int mover = game_get_player_on_turn_index(root);
    Move candidates[PEG_STATIC_POOL_SIZE];
    int pass_rank;
    const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg2feat candidate replay mismatch");
    }
    int64_t start = ctimer_monotonic_ns();
    Peg2RootContext prep;
    peg2_root_context_fill(root, &work, &prep);
    const double prep_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    int wide_count;
    const int scored =
        peg2_scored_candidates(root, root_id, &ranks, candidates, count, top_k,
                               wide, wide_moves, &wide_count, chosen, ids);
    float *rows =
        malloc_or_die(sizeof(float) * PEG2_ORDERED_PAIRS * PEG2_FEATURE_WIDTH);
    for (int slot = 0; slot < scored; slot++) {
      const int candidate = ids[slot];
      const Move *cand = chosen[slot];
      const int tiles = move_get_tiles_played(cand);
      const int n_drawn = tiles < 2 ? tiles : 2;
      float scalars[VALUE_NET_SCALARS];
      value_net_scalars_for_move(root, cand, &history, scalars);
      float public_row[VALUE_NET_SCALARS - 4 + 2];
      int col = 0;
      for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
        if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
          public_row[col++] = scalars[idx];
        }
      }
      const double after = value_net_spread_after_move(root, cand);
      public_row[col++] = (float)equity_to_double(move_get_equity(cand));
      public_row[col++] = (float)after;
      fwrite(public_row, sizeof(float), (size_t)col, public_out);
      const uint64_t touched =
          peg_world_lane_influence(game_get_board(root), cand);
      double cheap_ms = 0, net_ms = 0, weight_sum = 0;
      int net_calls = 0, filled = 0;
      for (int pair = 0; pair < prep.pairs; pair++) {
        const MachineLetter tile_a = prep.tile_a[pair];
        const MachineLetter tile_b = prep.tile_b[pair];
        const int orders = n_drawn == 1 && tile_a != tile_b ? 2 : 1;
        bool have_net = false;
        float net_cols[6] = {0};
        for (int order = 0; order < orders; order++) {
          const MachineLetter bag2[2] = {order == 0 ? tile_a : tile_b,
                                         order == 0 ? tile_b : tile_a};
          float *feature = &rows[(size_t)filled * PEG2_FEATURE_WIDTH];
          memset(feature, 0, sizeof(float) * PEG2_FEATURE_WIDTH);
          const double weight = n_drawn == 1
                                    ? (tile_a == tile_b ? 2.0 * prep.count[pair]
                                                        : prep.count[pair])
                                    : 2.0 * prep.count[pair];
          feature[0] = (float)(weight / PEG2_ORDERED_PAIRS);
          weight_sum += weight;
          start = ctimer_monotonic_ns();
          peg_world_after_candidate(root, prep.unseen, cand, bag2, n_drawn,
                                    world);
          if (game_over(world)) {
            // The mover went out with an empty bag: nothing follows.
            feature[14] = n_drawn == 1 ? bag2[0] : -1;
            feature[15] = n_drawn == 1 ? bag2[1] : -1;
            cheap_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
            filled++;
            continue;
          }
          peg2_opponent_top_reply(&prep, pair, touched, world, mover, &work,
                                  feature);
          bool out = false, pass;
          int greedy_tiles;
          if (n_drawn == 1) {
            // The mover's greedy reply after the opponent's top-scoring
            // reply, which draws the last tile.
            if (move_get_type(work.found) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
              play_move(work.found, world, NULL);
            } else {
              Move pass_move;
              move_set_as_pass(&pass_move);
              play_move(&pass_move, world, NULL);
            }
            if (!game_over(world)) {
              feature[5] = (float)greedy_endgame_move(
                  world, work.small, work.move, &out, &greedy_tiles, &pass);
            }
          } else {
            game_set_player_on_turn_index(world, mover);
            feature[5] = (float)greedy_endgame_move(
                world, work.small, work.move, &out, &greedy_tiles, &pass);
          }
          feature[6] = out;
          feature[14] = n_drawn == 1 ? bag2[0] : -1;
          feature[15] = n_drawn == 1 ? bag2[1] : -1;
          cheap_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
          if (n_drawn == 1 && net_features) {
            if (!have_net) {
              start = ctimer_monotonic_ns();
              peg_world_after_candidate(root, prep.unseen, cand, bag2, n_drawn,
                                        world);
              Move pick;
              double win, spread, value;
              peg2_reply_net_value(net, world, &pick, &win, &spread, &value);
              net_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
              net_calls++;
              const bool placed =
                  move_get_type(&pick) == GAME_EVENT_TILE_PLACEMENT_MOVE;
              net_cols[0] = 1;
              net_cols[1] = placed ? (float)win : 0;
              net_cols[2] = placed ? (float)spread : 0;
              net_cols[3] = placed ? (float)value : 0;
              net_cols[4] = (float)equity_to_int(move_get_score(&pick));
              net_cols[5] = placed ? (float)move_get_tiles_played(&pick) : 0;
              have_net = true;
              if (check) {
                // Information rule: the opponent's pick and values must not
                // change when the bag tile and the mover's drawn tile swap
                // (the opponent cannot tell them apart).
                const MachineLetter other[2] = {bag2[1], bag2[0]};
                peg_world_after_candidate(root, prep.unseen, cand, other,
                                          n_drawn, swapped_root);
                Move pick2;
                double win2, spread2, value2;
                peg2_reply_net_value(net, swapped_root, &pick2, &win2, &spread2,
                                     &value2);
                if (compare_moves(&pick, &pick2, true) != -1 ||
                    (placed && (win != win2 || spread != spread2))) {
                  log_fatal("peg2feat network features depend on hidden "
                            "tiles");
                }
              }
            }
            feature[7] = net_cols[0];
            feature[8] = net_cols[1];
            feature[9] = net_cols[2];
            feature[10] = net_cols[3];
            feature[11] = net_cols[4];
            feature[12] = net_cols[5];
            feature[13] = net_cols[5] == 0;
          }
          filled++;
        }
      }
      if (weight_sum != PEG2_ORDERED_PAIRS) {
        log_fatal("peg2feat world weights do not cover the unseen pool");
      }
      fwrite(rows, sizeof(float), (size_t)filled * PEG2_FEATURE_WIDTH,
             worlds_out);
      fprintf(cands, "%d,%d,%d,%d,%.9g,%.9g,%d,%.6g,%.6g,%.6g,%d\n", root_id,
              candidate, wide_count, tiles, after,
              equity_to_double(move_get_equity(cand)), filled,
              slot == 0 ? prep_ms : 0, cheap_ms, net_ms, net_calls);
    }
    free(rows);
    if (check) {
      // Information rule for the whole board: with the actual bag swapped for
      // two other unseen tiles (taken from the opponent's rack), the prep must
      // be identical; the features are built from the prep and public state.
      int actual[2] = {-1, -1}, found = 0;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        for (int copy = 0;
             copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml) &&
             found < 2;
             copy++) {
          actual[found++] = ml;
        }
      }
      MachineLetter swap2[2];
      int picked = 0;
      int rest[VALUE_NET_TILE_TYPES];
      memcpy(rest, prep.unseen, sizeof(rest));
      rest[actual[0]]--;
      rest[actual[1]]--;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && picked < 2; ml++) {
        while (rest[ml] > 0 && picked < 2 &&
               (ml != actual[0] && ml != actual[1])) {
          swap2[picked++] = (MachineLetter)ml;
          rest[ml]--;
        }
      }
      if (picked == 2) {
        Move pass;
        move_set_as_pass(&pass);
        peg_world_after_candidate(root, prep.unseen, &pass, swap2, 0,
                                  swapped_root);
        // swapped_root: same board and mover rack, the opponent holding the
        // old bag tiles, the bag holding swap2.
        bag_set_to_tiles(game_get_bag(swapped_root), swap2, 2);
        game_set_player_on_turn_index(swapped_root, mover);
        Peg2RootContext prep2;
        peg2_root_context_fill(swapped_root, &work, &prep2);
        if (prep2.pairs != prep.pairs ||
            memcmp(prep2.unseen, prep.unseen, sizeof(prep.unseen)) != 0) {
          log_fatal("peg2feat prep depends on hidden tiles");
        }
        for (int pair = 0; pair < prep.pairs; pair++) {
          for (int idx = 0; idx < PEG_WORLD_ROOT_LANES; idx++) {
            if (compare_moves(&prep.lane_best[pair][idx],
                              &prep2.lane_best[pair][idx], true) != -1 &&
                move_get_type(&prep.lane_best[pair][idx]) ==
                    GAME_EVENT_TILE_PLACEMENT_MOVE) {
              log_fatal("peg2feat prep depends on hidden tiles");
            }
          }
        }
      }
    }
    fflush(cands);
    fflush(public_out);
    fflush(worlds_out);
    printf("peg2feat root %d candidates %d\n", root_id, scored);
    fflush(stdout);
  }
  free(line);
  fclose(positions);
  fclose(cands);
  fclose(public_out);
  fclose(worlds_out);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  peg2_candidate_list_destroy(&ranks);
  move_list_destroy(wide);
  free(wide_moves);
  free(chosen);
  free(ids);
  peg1_scratch_destroy(&work);
  peg1_net_chooser_destroy(net);
  game_destroy(world);
  game_destroy(swapped_root);
  config_destroy(config);
}

void peg3_positions_mode(const StringSplitter *fields) {
  peg_positions_generate(fields, 3);
}
