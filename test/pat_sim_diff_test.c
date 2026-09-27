#include "pat_sim_diff_test.h"

#include "../src/def/bai_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_features.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Research harness: where does PAT disagree with a simulation, and which of
// its channels account for it?
//
// Positions come from static self-play with PSD_GEN_PAT (default CSW24),
// each game stopped at the first turn after the opening whose bag is at or
// below a threshold drawn uniformly from [PSD_MIN_BAG, PSD_MAX_BAG]. The
// candidates are the top PSD_CANDS moves by static equity without PAT and
// under each of PSD_PATS, and for each of those up to PSD_SAME_LEAVE other
// placements of the same tiles (highest plain equity first): pairs with one
// leave differ only on the board, which is what PAT prices. Each is simmed
// PSD_PLIES plies with plain static
// rollouts (no PAT for either side), PSD_ITERS iterations per move, every
// move seeing the same draws. One CSV row per candidate: its static equity
// without PAT and under each PAT file, the sim's equity (and its standard
// error and win%), the move's leave, and the PAT feature row, in the
// combination the first
// PAT file's weights apply, of the board the move leaves. The position's
// CGP goes to <PSD_OUT>.cgp.
//
// Environment:
//   PSD_OUT            CSV path (appended; positions already in it are
//                      skipped)
//   PSD_WORKER         worker index w; handles positions w, w + n, ...
//   PSD_NUM_WORKERS    n
//   PSD_DEADLINE       Unix time; no position starts at or after it
//   PSD_ITERS          sim iterations per move (default 10000)
//   PSD_PLIES          sim plies (default 4)
//   PSD_CANDS          candidates per ranking (default 6)
//   PSD_SAME_LEAVE     same-leave placements added per candidate (default 4)
//   PSD_PATS           comma-separated PAT names (default CSW24)
//   PSD_GEN_PAT        PAT for the self-play that makes positions
//   PSD_MAX_POSITIONS  stop after this position index (default 100000)
//   PSD_SEED           base seed (default 20260927)

enum {
  PSD_MIN_BAG = 8,
  PSD_MAX_BAG = 80,
  PSD_MAX_GEN_TURNS = 60,
  PSD_MAX_PATS = 4,
  PSD_UNION_CAP = 64,
  PSD_FULL_LIST_CAP = 10000,
  PSD_NAME_CAP = 64,
  PSD_NAME_LIST_CAP = 256,
};

static long psd_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  return value ? strtol(value, NULL, 10) : default_value;
}

static uint64_t psd_mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

// Marks which position indices the CSV already holds.
static void psd_load_done(const char *path, bool *done, long max_positions) {
  FILE *in = fopen(path, "r");
  if (!in) {
    return;
  }
  char line[65536];
  while (fgets(line, sizeof(line), in)) {
    char *end = NULL;
    const long pos = strtol(line, &end, 10);
    if (end != line && *end == ',' && pos >= 0 && pos < max_positions) {
      done[pos] = true;
    }
  }
  (void)fclose(in);
}

// Plays static self-play from a seeded deal until the bag first reaches
// the drawn threshold after the opening. Returns false if the game ended
// or ran long first.
static bool psd_make_position(Game *game, MoveList *move_list, uint64_t seed) {
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, (int)(seed & 1));
  draw_starting_racks(game);
  const int threshold =
      PSD_MIN_BAG + (int)(psd_mix(seed) % (PSD_MAX_BAG - PSD_MIN_BAG + 1));
  for (int turn = 0; turn < PSD_MAX_GEN_TURNS; turn++) {
    if (game_over(game)) {
      return false;
    }
    const int bag = bag_get_letters(game_get_bag(game));
    if (turn >= 2 && bag <= threshold && bag >= PSD_MIN_BAG) {
      return true;
    }
    if (bag < PSD_MIN_BAG) {
      return false;
    }
    play_move(get_top_equity_move(game, move_list), game, NULL);
  }
  return false;
}

static void psd_generate(const Game *game, MoveList *move_list) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
}

// Index of move in moves[0..count), or -1.
static int psd_find_move(Move *const *moves, int count, const Move *move) {
  for (int move_idx = 0; move_idx < count; move_idx++) {
    if (compare_moves_without_equity(moves[move_idx], move, true) == -1) {
      return move_idx;
    }
  }
  return -1;
}

// Adds the config's top cands moves to the union.
static void psd_add_top(const Game *game, int cands, Move **union_moves,
                        int *union_count) {
  MoveList *top_list = move_list_create(cands);
  psd_generate(game, top_list);
  for (int move_idx = 0; move_idx < move_list_get_count(top_list); move_idx++) {
    const Move *move = move_list_get_move(top_list, move_idx);
    if (*union_count < PSD_UNION_CAP &&
        psd_find_move(union_moves, *union_count, move) < 0) {
      move_copy(union_moves[(*union_count)++], move);
    }
  }
  move_list_destroy(top_list);
}

// Adds, for each of the first num_seeds union moves, up to same_leave other
// placements of the same tiles from full_list (sorted by plain equity).
static void psd_add_same_leave(const Game *game, const MoveList *full_list,
                               int num_seeds, int same_leave,
                               Move **union_moves, int *union_count) {
  for (int seed_idx = 0; seed_idx < num_seeds; seed_idx++) {
    if (move_get_type(union_moves[seed_idx]) !=
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    Rack seed_leave;
    get_leave_for_move(union_moves[seed_idx], game, &seed_leave);
    int added = 0;
    for (int list_idx = 0; list_idx < move_list_get_count(full_list) &&
                           added < same_leave && *union_count < PSD_UNION_CAP;
         list_idx++) {
      const Move *move = move_list_get_move(full_list, list_idx);
      if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      Rack leave;
      get_leave_for_move(move, game, &leave);
      if (!racks_are_equal(&leave, &seed_leave) ||
          psd_find_move(union_moves, *union_count, move) >= 0) {
        continue;
      }
      move_copy(union_moves[(*union_count)++], move);
      added++;
    }
  }
}

// Each union move's static equity under the config's evaluation.
static void psd_equities(const Game *game, MoveList *full_list,
                         Move *const *union_moves, int union_count,
                         double *equities) {
  move_list_reset(full_list);
  psd_generate(game, full_list);
  for (int move_idx = 0; move_idx < union_count; move_idx++) {
    equities[move_idx] = -1e9;
  }
  for (int list_idx = 0; list_idx < move_list_get_count(full_list);
       list_idx++) {
    const Move *move = move_list_get_move(full_list, list_idx);
    const int move_idx = psd_find_move(union_moves, union_count, move);
    if (move_idx >= 0) {
      equities[move_idx] = equity_to_double(move_get_equity(move));
    }
  }
}

void test_pat_sim_diff(void) {
  const char *out_path = getenv("PSD_OUT");
  if (!out_path) {
    log_fatal("set PSD_OUT");
  }
  const long worker = psd_env_long("PSD_WORKER", 0);
  const long num_workers = psd_env_long("PSD_NUM_WORKERS", 1);
  const long deadline = psd_env_long("PSD_DEADLINE", 0);
  const long iters = psd_env_long("PSD_ITERS", 10000);
  const int plies = (int)psd_env_long("PSD_PLIES", 4);
  const int cands = (int)psd_env_long("PSD_CANDS", 6);
  const int same_leave = (int)psd_env_long("PSD_SAME_LEAVE", 4);
  const long max_positions = psd_env_long("PSD_MAX_POSITIONS", 100000);
  const uint64_t base_seed = (uint64_t)psd_env_long("PSD_SEED", 20260927);
  const char *gen_pat = getenv("PSD_GEN_PAT") ? getenv("PSD_GEN_PAT") : "CSW24";
  char pat_list[PSD_NAME_LIST_CAP];
  (void)snprintf(pat_list, sizeof(pat_list), "%s",
                 getenv("PSD_PATS") ? getenv("PSD_PATS") : "CSW24");
  char pat_names[PSD_MAX_PATS][PSD_NAME_CAP];
  int num_pats = 0;
  for (char *token = strtok(pat_list, ","); token && num_pats < PSD_MAX_PATS;
       token = strtok(NULL, ",")) {
    (void)snprintf(pat_names[num_pats++], PSD_NAME_CAP, "%s", token);
  }

  const char *settings = "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity "
                         "-s2 equity -r1 all -r2 all -numplays 1 -threads 1";
  char *gen_command = get_formatted_string("%s -pat %s", settings, gen_pat);
  Config *gen_config = config_create_or_die(gen_command);
  free(gen_command);
  Config *plain_config = config_create_or_die(settings);
  Config *pat_configs[PSD_MAX_PATS];
  for (int pat_idx = 0; pat_idx < num_pats; pat_idx++) {
    char *command =
        get_formatted_string("%s -pat %s", settings, pat_names[pat_idx]);
    pat_configs[pat_idx] = config_create_or_die(command);
    free(command);
  }
  const char *empty_cgp =
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0";
  load_and_exec_config_or_die(gen_config, empty_cgp);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(plain_config, error_stack);
  assert(error_stack_is_empty(error_stack));
  Game *gen_game = config_get_game(gen_config);
  MoveList *gen_list = move_list_create(1);
  MoveList *full_list = move_list_create(PSD_FULL_LIST_CAP);
  MoveList *union_list = move_list_create(PSD_UNION_CAP);
  Move *union_moves[PSD_UNION_CAP];
  for (int move_idx = 0; move_idx < PSD_UNION_CAP; move_idx++) {
    union_moves[move_idx] = move_create();
  }
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;

  bool *done = calloc_or_die((size_t)max_positions, sizeof(bool));
  psd_load_done(out_path, done, max_positions);
  FILE *out = fopen(out_path, "a");
  assert(out);
  char *cgp_path = get_formatted_string("%s.cgp", out_path);
  FILE *cgp_out = fopen(cgp_path, "a");
  assert(cgp_out);
  free(cgp_path);
  if (ftell(out) == 0) {
    fprintf(out, "pos,bag,move,score,eq_plain");
    for (int pat_idx = 0; pat_idx < num_pats; pat_idx++) {
      fprintf(out, ",eq_%s", pat_names[pat_idx]);
    }
    fprintf(out, ",sim_eq,sim_eq_sem,sim_wp,leave");
    for (int feature_idx = 0; feature_idx < PAT_NUM_FEATURES; feature_idx++) {
      char name[PSD_NAME_CAP];
      pat_feature_name(feature_idx, name, sizeof(name));
      fprintf(out, ",%s", name);
    }
    fprintf(out, "\n");
    (void)fflush(out);
  }

  for (long pos = worker; pos < max_positions; pos += num_workers) {
    if (done[pos]) {
      continue;
    }
    if (deadline > 0 && (long)time(NULL) >= deadline) {
      break;
    }
    if (!psd_make_position(gen_game, gen_list,
                           psd_mix(base_seed ^ (uint64_t)pos))) {
      fprintf(out, "%ld,skip\n", pos);
      (void)fflush(out);
      continue;
    }
    const int bag = bag_get_letters(game_get_bag(gen_game));
    char *cgp = game_get_cgp(gen_game, true);
    char *cgp_command = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(plain_config, cgp_command);
    for (int pat_idx = 0; pat_idx < num_pats; pat_idx++) {
      load_and_exec_config_or_die(pat_configs[pat_idx], cgp_command);
    }
    fprintf(cgp_out, "%ld,%s\n", pos, cgp);
    (void)fflush(cgp_out);
    free(cgp_command);
    free(cgp);
    Game *plain_game = config_get_game(plain_config);

    int union_count = 0;
    psd_add_top(plain_game, cands, union_moves, &union_count);
    for (int pat_idx = 0; pat_idx < num_pats; pat_idx++) {
      psd_add_top(config_get_game(pat_configs[pat_idx]), cands, union_moves,
                  &union_count);
    }
    // The plain list, sorted by equity, supplies the same-leave placements.
    move_list_reset(full_list);
    psd_generate(plain_game, full_list);
    move_list_sort_moves(full_list);
    psd_add_same_leave(plain_game, full_list, union_count, same_leave,
                       union_moves, &union_count);
    double eq_plain[PSD_UNION_CAP];
    double eq_pat[PSD_MAX_PATS][PSD_UNION_CAP];
    psd_equities(plain_game, full_list, union_moves, union_count, eq_plain);
    for (int pat_idx = 0; pat_idx < num_pats; pat_idx++) {
      psd_equities(config_get_game(pat_configs[pat_idx]), full_list,
                   union_moves, union_count, eq_pat[pat_idx]);
    }

    move_list_reset(union_list);
    for (int move_idx = 0; move_idx < union_count; move_idx++) {
      move_list_add_move(union_list, union_moves[move_idx]);
    }
    ThreadControl *thread_control = thread_control_create();
    thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
    SimArgs sim_args = {0};
    sim_args_fill(plies, union_list, union_count, NULL,
                  config_get_win_pcts(plain_config),
                  /*inference_results=*/NULL, thread_control, plain_game,
                  /*sim_with_inference=*/false, /*use_heat_map=*/false,
                  /*num_threads=*/1, /*print_interval=*/0,
                  /*max_num_display_plays=*/union_count,
                  /*max_num_display_plies=*/plies,
                  /*seed=*/psd_mix(base_seed + 104729 * (uint64_t)pos),
                  /*max_iterations=*/(uint64_t)iters * (uint64_t)union_count,
                  /*min_play_iterations=*/(uint64_t)iters, /*scond=*/100.0,
                  BAI_THRESHOLD_NONE, /*time_limit_seconds=*/0.0,
                  BAI_SAMPLING_RULE_ROUND_ROBIN, /*cutoff=*/0.0, 1.0, 0.5,
                  100.0, /*use_margin_forecast=*/false,
                  /*inference_args=*/NULL, &sim_args);
    sim_args.pat_rollout_disabled = true;
    simulate(&sim_args, &sim_ctx, sim_results, error_stack);
    assert(error_stack_is_empty(error_stack));
    thread_control_destroy(thread_control);
    double sim_eq[PSD_UNION_CAP];
    double sim_sem[PSD_UNION_CAP];
    double sim_wp[PSD_UNION_CAP];
    for (int move_idx = 0; move_idx < union_count; move_idx++) {
      sim_eq[move_idx] = 0.0;
      sim_sem[move_idx] = -1.0;
      sim_wp[move_idx] = -1.0;
    }
    for (int play_idx = 0;
         play_idx < sim_results_get_number_of_plays(sim_results); play_idx++) {
      const SimmedPlay *simmed_play =
          sim_results_get_simmed_play(sim_results, play_idx);
      const int move_idx = psd_find_move(union_moves, union_count,
                                         simmed_play_get_move(simmed_play));
      assert(move_idx >= 0);
      const Stat *eq_stat = simmed_play_get_equity_stat(simmed_play);
      sim_eq[move_idx] = stat_get_mean(eq_stat);
      sim_sem[move_idx] = stat_get_sem(eq_stat);
      sim_wp[move_idx] =
          stat_get_mean(simmed_play_get_win_pct_stat(simmed_play));
    }

    const Game *feature_game = config_get_game(pat_configs[0]);
    const PATWeights *feature_pat =
        player_get_pat(game_get_player(feature_game, 0));
    for (int move_idx = 0; move_idx < union_count; move_idx++) {
      const Move *move = union_moves[move_idx];
      double features[PAT_NUM_FEATURES] = {0};
      if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        Rack leave;
        get_leave_for_move(move, feature_game, &leave);
        Game *after = game_duplicate(feature_game);
        play_move(move, after, NULL);
        pat_extract_features_combined(
            board_get_readonly_lanes(game_get_board(after), 0),
            game_get_ld(after), &leave, feature_pat, RACK_SIZE, features);
        game_destroy(after);
      }
      StringBuilder *sb = string_builder_create();
      string_builder_add_formatted_string(sb, "%ld,%d,", pos, bag);
      string_builder_add_ucgi_move(sb, move, game_get_board(plain_game),
                                   game_get_ld(plain_game));
      string_builder_add_formatted_string(sb, ",%d,%.3f",
                                          equity_to_int(move_get_score(move)),
                                          eq_plain[move_idx]);
      for (int pat_idx = 0; pat_idx < num_pats; pat_idx++) {
        string_builder_add_formatted_string(sb, ",%.3f",
                                            eq_pat[pat_idx][move_idx]);
      }
      string_builder_add_formatted_string(sb, ",%.4f,%.4f,%.6f,",
                                          sim_eq[move_idx], sim_sem[move_idx],
                                          sim_wp[move_idx]);
      Rack move_leave;
      get_leave_for_move(move, plain_game, &move_leave);
      string_builder_add_rack(sb, &move_leave, game_get_ld(plain_game), false);
      for (int feature_idx = 0; feature_idx < PAT_NUM_FEATURES; feature_idx++) {
        if (features[feature_idx] == 0.0) {
          string_builder_add_string(sb, ",0");
        } else {
          string_builder_add_formatted_string(sb, ",%.3f",
                                              features[feature_idx]);
        }
      }
      string_builder_add_string(sb, "\n");
      fputs(string_builder_peek(sb), out);
      string_builder_destroy(sb);
    }
    (void)fflush(out);
  }

  (void)fclose(out);
  (void)fclose(cgp_out);
  for (int move_idx = 0; move_idx < PSD_UNION_CAP; move_idx++) {
    move_destroy(union_moves[move_idx]);
  }
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(union_list);
  move_list_destroy(full_list);
  move_list_destroy(gen_list);
  free(done);
  error_stack_destroy(error_stack);
  for (int pat_idx = 0; pat_idx < num_pats; pat_idx++) {
    config_destroy(pat_configs[pat_idx]);
  }
  config_destroy(plain_config);
  config_destroy(gen_config);
}
