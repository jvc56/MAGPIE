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
// them: 4 plies, every PAT class in rollouts, a fixed number of iterations
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
//   PCC_PLIES          sim plies (default 4)
//   PCC_SEED           base seed (default 20260926)

enum {
  PCC_NUM_K = 7,
  PCC_MIN_BAG = 5,
  PCC_MAX_BAG = 80,
  PCC_MAX_GEN_TURNS = 60,
  PCC_UNION_CAP = PCC_NUM_K + 1,
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
  const long deadline = pcc_env_long("PCC_DEADLINE", 0);
  const double budget = pcc_env_double("PCC_BUDGET", 13.0);
  const long oracle_iters = pcc_env_long("PCC_ORACLE_ITERS", 2000);
  const long max_positions = pcc_env_long("PCC_MAX_POSITIONS", 100000);
  const int plies = (int)pcc_env_long("PCC_PLIES", 4);
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
    for (int k_idx = 0; k_idx < PCC_NUM_K; k_idx++) {
      fprintf(out, ",k%d_cands,k%d_move,k%d_iters,k%d_wp,k%d_eq", pcc_ks[k_idx],
              pcc_ks[k_idx], pcc_ks[k_idx], pcc_ks[k_idx], pcc_ks[k_idx]);
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
  Move *chosen[PCC_NUM_K];
  for (int k_idx = 0; k_idx < PCC_NUM_K; k_idx++) {
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

    int num_cands[PCC_NUM_K];
    uint64_t iters[PCC_NUM_K];
    for (int k_idx = 0; k_idx < PCC_NUM_K; k_idx++) {
      PlayChooserStrategy strategy = {
          .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
          .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
          .sim_plies = plies,
          .sim_max_candidates = pcc_ks[k_idx],
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
      MoveList *count_list = move_list_create(pcc_ks[k_idx]);
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
    for (int k_idx = 0; k_idx < PCC_NUM_K; k_idx++) {
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
          plies, union_list, union_count, NULL, win_pcts,
          /*inference_results=*/NULL, thread_control, game,
          /*sim_with_inference=*/false, /*use_heat_map=*/false,
          /*num_threads=*/1, /*print_interval=*/0,
          /*max_num_display_plays=*/union_count,
          /*max_num_display_plies=*/plies,
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
    for (int k_idx = 0; k_idx < PCC_NUM_K; k_idx++) {
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
  for (int k_idx = 0; k_idx < PCC_NUM_K; k_idx++) {
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
