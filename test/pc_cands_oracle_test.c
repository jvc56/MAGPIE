#include "pc_cands_oracle_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Research harness: which number of sim candidates should PlayChooser use?
//
// Positions come from static PAT self-play (both players CSW24.pat), each
// game stopped at the first turn (after the opening) whose bag is at or
// below a threshold drawn uniformly from [PCC_MIN_BAG, PCC_MAX_BAG], so
// positions spread across the midgame. For each candidate count K,
// PlayChooser picks a move at that position exactly as in a game: top K by
// static equity with PAT, a 4-ply sim with BAI top-two sampling for a
// fixed per-move time budget on one thread, rollouts using the player's
// rollout classes (tws,windows on 15x15). The chosen moves (plus the
// static-best move) are then scored by one long oracle sim restricted to
// them: PCCANDS_ORACLE_PLIES plies (default 4), every PAT class in
// rollouts, a fixed number of iterations
// per move, and the same seed for every move so they see the same draws.
//
// Environment:
//   PCC_OUT            CSV path (appended; positions already in it are skipped)
//   PCC_WORKER         worker index w; handles positions w, w + n, w + 2n, ...
//   PCC_NUM_WORKERS    n
//   PCC_DEADLINE       Unix time; no position starts at or after it
//   PCC_BUDGET         seconds per PlayChooser move (default 13)
//   PCC_ORACLE_ITERS   oracle iterations per move (default 2000)
//   PCC_MAX_POSITIONS  stop after this position index (default 100000)
//   PCC_PLIES          sim plies when PCCANDS_PLIES_LIST is unset (default 4)
//   PCC_SEED           base seed (default 20260926)
//   PCCANDS_K_LIST     comma-separated candidate counts (default
//                      5,8,12,15,20,30,45)
//   PCCANDS_PLIES_LIST comma-separated sim plies (default PCC_PLIES)
//   PCCANDS_MINP_LIST  comma-separated minimum samples per candidate
//                      (default 1)
//   PCCANDS_ORACLE_PLIES  oracle sim plies (default the first sim plies)
//   PCCANDS_DEADLINE   same as PCC_DEADLINE, which it overrides
//
// Every (K, plies, minimum samples) triple is a setting. CSV columns are
// labeled by the dimensions that vary: k<K>, p<plies>, m<minimum samples>
// (k<K> when none vary).

enum {
  PCC_NUM_K = 7,
  PCC_MAX_SETTINGS = 16,
  PCC_LABEL_CAP = 16,
  PCC_MIN_BAG = 5,
  PCC_MAX_BAG = 80,
  PCC_MAX_GEN_TURNS = 60,
  PCC_UNION_CAP = PCC_MAX_SETTINGS + 1,
  PCC_LINE_CAP = 1 << 16,
};

static const int pcc_ks[PCC_NUM_K] = {5, 8, 12, 15, 20, 30, 45};

static long pcc_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  return value ? strtol(value, NULL, 10) : default_value;
}

static double pcc_env_double(const char *name, double default_value) {
  const char *value = getenv(name);
  return value ? strtod(value, NULL) : default_value;
}

// Parses a comma-separated list of positive integers into values (at most
// cap); falls back to defaults when the variable is unset. Returns the count.
static int pcc_env_list(const char *name, const int *defaults, int num_defaults,
                        int *values, int cap) {
  const char *value = getenv(name);
  if (!value) {
    for (int idx = 0; idx < num_defaults; idx++) {
      values[idx] = defaults[idx];
    }
    return num_defaults;
  }
  int count = 0;
  const char *cursor = value;
  while (*cursor != '\0' && count < cap) {
    char *end = NULL;
    const long parsed = strtol(cursor, &end, 10);
    if (end == cursor || parsed <= 0) {
      log_fatal("invalid %s: %s", name, value);
    }
    values[count++] = (int)parsed;
    cursor = (*end == ',') ? end + 1 : end;
  }
  return count;
}

static uint64_t pcc_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Marks which position indices the CSV already holds.
static void pcc_load_done(const char *path, bool *done, long max_positions) {
  FILE *file = fopen(path, "r");
  if (!file) {
    return;
  }
  char *line = malloc_or_die(PCC_LINE_CAP);
  while (fgets(line, PCC_LINE_CAP, file)) {
    char *end = NULL;
    const long index = strtol(line, &end, 10);
    if (end != line && index >= 0 && index < max_positions) {
      done[index] = true;
    }
  }
  free(line);
  (void)fclose(file);
}

// Plays static PAT self-play from a seeded deal until the bag first reaches
// the drawn threshold after the opening. Returns false if the game ended or
// ran long first.
static bool pcc_make_position(Game *game, MoveList *move_list, uint64_t seed,
                              int *turn_out) {
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, (int)(seed & 1));
  draw_starting_racks(game);
  const int threshold =
      PCC_MIN_BAG + (int)(pcc_mix(seed) % (PCC_MAX_BAG - PCC_MIN_BAG + 1));
  for (int turn = 0; turn < PCC_MAX_GEN_TURNS; turn++) {
    if (game_over(game)) {
      return false;
    }
    const int bag = bag_get_letters(game_get_bag(game));
    if (turn >= 2 && bag <= threshold && bag >= PCC_MIN_BAG) {
      *turn_out = turn;
      return true;
    }
    if (bag < PCC_MIN_BAG) {
      return false;
    }
    const Move *move = get_top_equity_move(game, move_list);
    play_move(move, game, NULL);
  }
  return false;
}

static void pcc_add_move_string(StringBuilder *sb, const Game *game,
                                const Move *move) {
  string_builder_add_ucgi_move(sb, move, game_get_board(game),
                               game_get_ld(game));
}

// Index of move in moves[0..count), or -1.
static int pcc_find_move(Move *const *moves, int count, const Move *move) {
  for (int move_idx = 0; move_idx < count; move_idx++) {
    if (compare_moves_without_equity(moves[move_idx], move, true) == -1) {
      return move_idx;
    }
  }
  return -1;
}

void test_pc_cands_oracle(void) {
  const char *out_path = getenv("PCC_OUT");
  if (!out_path) {
    log_fatal("set PCC_OUT");
  }
  const long worker = pcc_env_long("PCC_WORKER", 0);
  const long num_workers = pcc_env_long("PCC_NUM_WORKERS", 1);
  const long deadline =
      pcc_env_long("PCCANDS_DEADLINE", pcc_env_long("PCC_DEADLINE", 0));
  const double budget = pcc_env_double("PCC_BUDGET", 13.0);
  const long oracle_iters = pcc_env_long("PCC_ORACLE_ITERS", 2000);
  const long max_positions = pcc_env_long("PCC_MAX_POSITIONS", 100000);
  const int default_plies = (int)pcc_env_long("PCC_PLIES", 4);
  int k_list[PCC_MAX_SETTINGS];
  const int num_k = pcc_env_list("PCCANDS_K_LIST", pcc_ks, PCC_NUM_K, k_list,
                                 PCC_MAX_SETTINGS);
  int plies_list[PCC_MAX_SETTINGS];
  const int num_plies = pcc_env_list("PCCANDS_PLIES_LIST", &default_plies, 1,
                                     plies_list, PCC_MAX_SETTINGS);
  const int oracle_plies =
      (int)pcc_env_long("PCCANDS_ORACLE_PLIES", plies_list[0]);
  const int default_minp = 1;
  int minp_list[PCC_MAX_SETTINGS];
  const int num_minp = pcc_env_list("PCCANDS_MINP_LIST", &default_minp, 1,
                                    minp_list, PCC_MAX_SETTINGS);
  int setting_k[PCC_MAX_SETTINGS];
  int setting_plies[PCC_MAX_SETTINGS];
  int setting_minp[PCC_MAX_SETTINGS];
  char setting_label[PCC_MAX_SETTINGS][PCC_LABEL_CAP];
  int num_settings = 0;
  for (int k_idx = 0; k_idx < num_k; k_idx++) {
    for (int plies_idx = 0; plies_idx < num_plies; plies_idx++) {
      for (int minp_idx = 0; minp_idx < num_minp; minp_idx++) {
        if (num_settings == PCC_MAX_SETTINGS) {
          log_fatal("too many (K, plies, minimum samples) settings; the cap "
                    "is %d",
                    PCC_MAX_SETTINGS);
        }
        setting_k[num_settings] = k_list[k_idx];
        setting_plies[num_settings] = plies_list[plies_idx];
        setting_minp[num_settings] = minp_list[minp_idx];
        char *label = setting_label[num_settings];
        label[0] = '\0';
        size_t used = 0;
        if (num_k > 1 || (num_plies == 1 && num_minp == 1)) {
          used += (size_t)snprintf(label + used, PCC_LABEL_CAP - used, "k%d",
                                   k_list[k_idx]);
        }
        if (num_plies > 1) {
          used += (size_t)snprintf(label + used, PCC_LABEL_CAP - used, "p%d",
                                   plies_list[plies_idx]);
        }
        if (num_minp > 1) {
          (void)snprintf(label + used, PCC_LABEL_CAP - used, "m%d",
                         minp_list[minp_idx]);
        }
        num_settings++;
      }
    }
  }
  const uint64_t base_seed = (uint64_t)pcc_env_long("PCC_SEED", 20260926);

  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -pat CSW24 -s1 equity -s2 equity -r1 best "
      "-r2 best -numplays 1 -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  WinPct *win_pcts = config_get_win_pcts(config);
  Game *game = config_get_game(config);
  MoveList *gen_list = move_list_create(1);

  bool *done = calloc_or_die((size_t)max_positions, sizeof(bool));
  pcc_load_done(out_path, done, max_positions);
  FILE *out = fopen(out_path, "a");
  assert(out);
  if (ftell(out) == 0) {
    fprintf(out, "pos,turn,bag,union,oracle_best_wp,oracle_best_eq,"
                 "static_move,static_wp,static_eq");
    for (int setting_idx = 0; setting_idx < num_settings; setting_idx++) {
      const char *label = setting_label[setting_idx];
      fprintf(out, ",%s_cands,%s_move,%s_iters,%s_wp,%s_eq", label, label,
              label, label, label);
    }
    fprintf(out, ",oracle_wp_sem_max,seconds\n");
    (void)fflush(out);
  }

  play_chooser_benchmark_reset();
  SimResults *oracle_results = sim_results_create(0.0);
  SimCtx *oracle_ctx = NULL;
  MoveList *union_list = move_list_create(PCC_UNION_CAP);
  Move *union_moves[PCC_UNION_CAP];
  for (int move_idx = 0; move_idx < PCC_UNION_CAP; move_idx++) {
    union_moves[move_idx] = move_create();
  }
  Move *chosen[PCC_MAX_SETTINGS];
  for (int k_idx = 0; k_idx < num_settings; k_idx++) {
    chosen[k_idx] = move_create();
  }
  Move *static_move = move_create();

  for (long pos = worker; pos < max_positions; pos += num_workers) {
    if (done[pos]) {
      continue;
    }
    if (deadline > 0 && (long)time(NULL) >= deadline) {
      break;
    }
    const time_t start = time(NULL);
    int turn = 0;
    if (!pcc_make_position(game, gen_list, pcc_mix(base_seed ^ (uint64_t)pos),
                           &turn)) {
      fprintf(out, "%ld,skip\n", pos);
      (void)fflush(out);
      continue;
    }
    const int bag = bag_get_letters(game_get_bag(game));
    move_copy(static_move, get_top_equity_move(game, gen_list));

    int num_cands[PCC_MAX_SETTINGS];
    uint64_t iters[PCC_MAX_SETTINGS];
    for (int k_idx = 0; k_idx < num_settings; k_idx++) {
      PlayChooserStrategy strategy = {
          .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
          .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
          .sim_plies = setting_plies[k_idx],
          .sim_max_candidates = setting_k[k_idx],
          .sim_min_play_iterations = (uint64_t)setting_minp[k_idx],
          .fixed_seconds_per_move = budget,
          .win_pcts = win_pcts,
          .num_threads = 1,
          .utility_w_winpct = 1.0,
          .utility_w_spread = 0.5,
          .utility_spread_scale = 100.0,
          .pat_rollout_disabled = false,
          .pat_rollout_disabled_classes_mask =
              PAT_CLASS_MASK_ALL & ~PAT_CLASS_MASK_ROLLOUT_DEFAULT,
          .seed = pcc_mix(base_seed + 7919 * (uint64_t)pos + (uint64_t)k_idx),
      };
      PlayChooser *play_chooser = play_chooser_create(&strategy);
      Game *game_copy = game_duplicate(game);
      PlayChooserBenchmarkStats before;
      play_chooser_benchmark_get(&before);
      play_chooser_choose_move(play_chooser, game_copy, chosen[k_idx],
                               error_stack);
      assert(error_stack_is_empty(error_stack));
      PlayChooserBenchmarkStats after;
      play_chooser_benchmark_get(&after);
      iters[k_idx] = after.sim_iterations - before.sim_iterations;
      // The candidates PlayChooser had: the top K by static equity, or fewer.
      MoveList *count_list = move_list_create(setting_k[k_idx]);
      const MoveGenArgs count_args = {
          .game = game_copy,
          .move_list = count_list,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&count_args);
      num_cands[k_idx] = move_list_get_count(count_list);
      move_list_destroy(count_list);
      game_destroy(game_copy);
      play_chooser_destroy(play_chooser);
    }

    // The distinct moves to score: every K's choice plus the static best.
    int union_count = 0;
    move_copy(union_moves[union_count++], static_move);
    for (int k_idx = 0; k_idx < num_settings; k_idx++) {
      if (pcc_find_move(union_moves, union_count, chosen[k_idx]) < 0) {
        move_copy(union_moves[union_count++], chosen[k_idx]);
      }
    }
    move_list_reset(union_list);
    for (int move_idx = 0; move_idx < union_count; move_idx++) {
      move_list_add_move(union_list, union_moves[move_idx]);
    }

    double wp[PCC_UNION_CAP];
    double eq[PCC_UNION_CAP];
    double sem_max = 0.0;
    if (union_count == 1) {
      wp[0] = 0.0;
      eq[0] = 0.0;
    } else {
      ThreadControl *thread_control = thread_control_create();
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
      SimArgs sim_args = {0};
      sim_args_fill(
          oracle_plies, union_list, union_count, NULL, win_pcts,
          /*inference_results=*/NULL, thread_control, game,
          /*sim_with_inference=*/false, /*use_heat_map=*/false,
          /*num_threads=*/1, /*print_interval=*/0,
          /*max_num_display_plays=*/union_count,
          /*max_num_display_plies=*/oracle_plies,
          /*seed=*/pcc_mix(base_seed + 104729 * (uint64_t)pos),
          /*max_iterations=*/(uint64_t)oracle_iters * (uint64_t)union_count,
          /*min_play_iterations=*/(uint64_t)oracle_iters, /*scond=*/100.0,
          BAI_THRESHOLD_NONE, /*time_limit_seconds=*/0.0,
          BAI_SAMPLING_RULE_ROUND_ROBIN, /*cutoff=*/0.0, 1.0, 0.5, 100.0,
          /*use_margin_forecast=*/false, /*inference_args=*/NULL, &sim_args);
      sim_args.pat_rollout_disabled = false;
      sim_args.pat_rollout_disabled_classes_mask = 0;
      simulate(&sim_args, &oracle_ctx, oracle_results, error_stack);
      assert(error_stack_is_empty(error_stack));
      thread_control_destroy(thread_control);
      const int num_simmed = sim_results_get_number_of_plays(oracle_results);
      for (int move_idx = 0; move_idx < union_count; move_idx++) {
        wp[move_idx] = -1.0;
        eq[move_idx] = 0.0;
      }
      for (int play_idx = 0; play_idx < num_simmed; play_idx++) {
        const SimmedPlay *simmed_play =
            sim_results_get_simmed_play(oracle_results, play_idx);
        const int move_idx = pcc_find_move(union_moves, union_count,
                                           simmed_play_get_move(simmed_play));
        assert(move_idx >= 0);
        const Stat *wp_stat = simmed_play_get_win_pct_stat(simmed_play);
        wp[move_idx] = stat_get_mean(wp_stat);
        eq[move_idx] = stat_get_mean(simmed_play_get_equity_stat(simmed_play));
        if (stat_get_sem(wp_stat) > sem_max) {
          sem_max = stat_get_sem(wp_stat);
        }
      }
    }
    double best_wp = wp[0];
    double best_eq = eq[0];
    for (int move_idx = 1; move_idx < union_count; move_idx++) {
      if (wp[move_idx] > best_wp) {
        best_wp = wp[move_idx];
      }
      if (eq[move_idx] > best_eq) {
        best_eq = eq[move_idx];
      }
    }

    StringBuilder *sb = string_builder_create();
    string_builder_add_formatted_string(sb, "%ld,%d,%d,%d,%.6f,%.4f,", pos,
                                        turn, bag, union_count, best_wp,
                                        best_eq);
    pcc_add_move_string(sb, game, static_move);
    string_builder_add_formatted_string(sb, ",%.6f,%.4f", wp[0], eq[0]);
    for (int k_idx = 0; k_idx < num_settings; k_idx++) {
      const int move_idx =
          pcc_find_move(union_moves, union_count, chosen[k_idx]);
      string_builder_add_formatted_string(sb, ",%d,", num_cands[k_idx]);
      pcc_add_move_string(sb, game, chosen[k_idx]);
      string_builder_add_formatted_string(sb, ",%llu,%.6f,%.4f",
                                          (unsigned long long)iters[k_idx],
                                          wp[move_idx], eq[move_idx]);
    }
    string_builder_add_formatted_string(sb, ",%.6f,%ld\n", sem_max,
                                        (long)(time(NULL) - start));
    fputs(string_builder_peek(sb), out);
    (void)fflush(out);
    string_builder_destroy(sb);
  }

  (void)fclose(out);
  for (int move_idx = 0; move_idx < PCC_UNION_CAP; move_idx++) {
    move_destroy(union_moves[move_idx]);
  }
  for (int k_idx = 0; k_idx < num_settings; k_idx++) {
    move_destroy(chosen[k_idx]);
  }
  move_destroy(static_move);
  move_list_destroy(union_list);
  sim_ctx_destroy(oracle_ctx);
  sim_results_destroy(oracle_results);
  move_list_destroy(gen_list);
  free(done);
  error_stack_destroy(error_stack);
  config_destroy(config);
}
