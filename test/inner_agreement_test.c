#include "inner_agreement_test.h"

#include "../src/def/bai_defs.h"
#include "../src/def/board_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/game.h"
#include "../src/ent/heat_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/random_variable.h"
#include "../src/impl/simmer.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INNERAGREE_DEFAULT_GAMES 5
#define INNERAGREE_DEFAULT_TLIM 20.0
#define INNERAGREE_DEFAULT_OUTER_K 100
#define INNERAGREE_DEFAULT_OUTER_MIN_ITER 10
#define INNERAGREE_DEFAULT_OUTER_PLIES 4
#define INNERAGREE_DEFAULT_INNER_K 40
#define INNERAGREE_DEFAULT_INNER_FLOOR 3
#define INNERAGREE_DEFAULT_INNER_MAX 250
#define INNERAGREE_DEFAULT_INNER_PLIES 2
#define INNERAGREE_DEFAULT_INNER_STOPZ 2.326
#define INNERAGREE_DEFAULT_W_WINPCT 1.0
#define INNERAGREE_DEFAULT_W_SPREAD 0.5
#define INNERAGREE_DEFAULT_SPREAD_SCALE 100.0
#define INNERAGREE_DEFAULT_NUM_THREADS 10

// Run an outer flat-BAI sim (PLY_STRATEGY_STATIC leaves). Used by the
// augment mode to capture flat-variant per-arm/per-ply data alongside the
// nested variant for the details JSONL output.
static const Move *run_outer_flat(
    Game *game, MoveList *move_list, SimResults *sim_results, SimCtx **sim_ctx,
    WinPct *win_pcts, ThreadControl *tc, int outer_K, int outer_min_iter,
    int outer_plies, int num_threads, double tlim, bool use_heat_map,
    double w_winpct, double w_spread, double spread_scale,
    ErrorStack *error_stack) {
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  if (move_list_get_count(move_list) <= 1) {
    return move_list_get_move(move_list, 0);
  }
  SimArgs sim_args;
  sim_args_fill(outer_plies, move_list, outer_K, NULL, win_pcts, NULL, tc, game,
                false, use_heat_map, num_threads, 0, outer_K, outer_plies, 0,
                UINT64_MAX, (uint64_t)outer_min_iter, 101.0, BAI_THRESHOLD_NONE,
                tlim, BAI_SAMPLING_RULE_TOP_TWO_IDS, 0.0, NULL, &sim_args);
  sim_args.utility_w_winpct = w_winpct;
  sim_args.utility_w_spread = w_spread;
  sim_args.utility_spread_scale = spread_scale;
  sim_args.num_fidelity_levels = 1;
  sim_args.fidelity_levels[0] = (FidelityLevel){
      .sample_limit = UINT64_MAX,
      .sample_minimum = (uint64_t)outer_min_iter,
      .time_limit_seconds = tlim,
      .ply_strategy = PLY_STRATEGY_STATIC,
      .inner_w_winpct = 1.0,
      .inner_w_spread = 0.0,
      .inner_spread_scale = 100.0,
  };
  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  simulate(&sim_args, sim_ctx, sim_results, error_stack);
  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0) {
    best_arm = 0;
  }
  return simmed_play_get_move(
      sim_results_get_simmed_play(sim_results, best_arm));
}

// Run an outer nested-sim BAI (PLY_STRATEGY_NESTED_SIM leaves). Used as the
// instrumentation pass: every leaf eval triggers an inner sim call whose
// agree-vs-cand0 and util loss get tallied into the worker's InnerDiag
// counters. Returns picked move pointer.
static const Move *run_outer_nested(
    Game *game, MoveList *move_list, SimResults *sim_results, SimCtx **sim_ctx,
    WinPct *win_pcts, ThreadControl *tc, int outer_K, int outer_min_iter,
    int outer_plies, int num_threads, double tlim, bool use_heat_map,
    int inner_K, int inner_floor, int inner_max, int inner_plies,
    double inner_stop_z, double w_winpct, double w_spread, double spread_scale,
    ErrorStack *error_stack) {
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  if (move_list_get_count(move_list) <= 1) {
    return move_list_get_move(move_list, 0);
  }

  SimArgs sim_args;
  sim_args_fill(outer_plies, move_list, outer_K, NULL, win_pcts, NULL, tc, game,
                false, use_heat_map, num_threads, 0, outer_K, outer_plies, 0,
                UINT64_MAX, (uint64_t)outer_min_iter, 101.0, BAI_THRESHOLD_NONE,
                tlim, BAI_SAMPLING_RULE_TOP_TWO_IDS, 0.0, NULL, &sim_args);
  sim_args.utility_w_winpct = w_winpct;
  sim_args.utility_w_spread = w_spread;
  sim_args.utility_spread_scale = spread_scale;
  sim_args.num_fidelity_levels = 1;
  sim_args.fidelity_levels[0] = (FidelityLevel){
      .sample_limit = UINT64_MAX,
      .sample_minimum = (uint64_t)outer_min_iter,
      .time_limit_seconds = tlim,
      .ply_strategy = PLY_STRATEGY_NESTED_SIM,
      .nested_candidates = inner_K,
      .nested_rollouts = inner_floor,
      .nested_plies = inner_plies,
      .nested_max_samples = inner_max,
      .nested_stop_z = inner_stop_z,
      .inner_w_winpct = w_winpct,
      .inner_w_spread = w_spread,
      .inner_spread_scale = spread_scale,
  };

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0) {
    best_arm = 0;
  }
  return simmed_play_get_move(
      sim_results_get_simmed_play(sim_results, best_arm));
}

static int env_int(const char *name, int default_value) {
  const char *v = getenv(name);
  if (!v || !*v) {
    return default_value;
  }
  return atoi(v);
}

static double env_double(const char *name, double default_value) {
  const char *v = getenv(name);
  if (!v || !*v) {
    return default_value;
  }
  return atof(v);
}

void test_inner_agreement(void) {
  const int num_games = env_int("INNERAGREE_GAMES", INNERAGREE_DEFAULT_GAMES);
  const double tlim = env_double("INNERAGREE_TLIM", INNERAGREE_DEFAULT_TLIM);
  const int outer_K =
      env_int("INNERAGREE_OUTER_K", INNERAGREE_DEFAULT_OUTER_K);
  const int outer_min_iter =
      env_int("INNERAGREE_OUTER_MIN_ITER", INNERAGREE_DEFAULT_OUTER_MIN_ITER);
  const int outer_plies =
      env_int("INNERAGREE_OUTER_PLIES", INNERAGREE_DEFAULT_OUTER_PLIES);
  const int inner_K =
      env_int("INNERAGREE_INNER_K", INNERAGREE_DEFAULT_INNER_K);
  const int inner_floor =
      env_int("INNERAGREE_INNER_FLOOR", INNERAGREE_DEFAULT_INNER_FLOOR);
  const int inner_max =
      env_int("INNERAGREE_INNER_MAX", INNERAGREE_DEFAULT_INNER_MAX);
  const int inner_plies =
      env_int("INNERAGREE_INNER_PLIES", INNERAGREE_DEFAULT_INNER_PLIES);
  const double inner_stop_z =
      env_double("INNERAGREE_INNER_STOPZ", INNERAGREE_DEFAULT_INNER_STOPZ);
  const double w_winpct =
      env_double("INNERAGREE_W_WINPCT", INNERAGREE_DEFAULT_W_WINPCT);
  const double w_spread =
      env_double("INNERAGREE_W_SPREAD", INNERAGREE_DEFAULT_W_SPREAD);
  const double spread_scale = env_double("INNERAGREE_SPREAD_SCALE",
                                         INNERAGREE_DEFAULT_SPREAD_SCALE);
  const int num_threads =
      env_int("INNERAGREE_THREADS", INNERAGREE_DEFAULT_NUM_THREADS);
  const uint64_t base_seed =
      (uint64_t)env_int("INNERAGREE_SEED", (int)time(NULL));
  const char *csv_path = getenv("INNERAGREE_CSV");

  printf("\n");
  printf("================================================\n");
  printf("  Inner-pick Agreement / Loss Per-Root Aggregator\n");
  printf("  games=%d  threads=%d  seed=%" PRIu64 "\n", num_games, num_threads,
         base_seed);
  printf("  sim = outer NESTED K=%d, %.1fs/turn (inner K=%d floor=%d "
         "max=%d plies=%d stopz=%.3f)\n",
         outer_K, tlim, inner_K, inner_floor, inner_max, inner_plies,
         inner_stop_z);
  printf("  blend = w_winpct=%.2f w_spread=%.2f spread_scale=%.1f\n", w_winpct,
         w_spread, spread_scale);
  printf("  csv=%s\n", csv_path ? csv_path : "(none)");
  printf("================================================\n\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity -r1 all -r2 "
      "all -numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, load_es);
  if (!error_stack_is_empty(load_es)) {
    error_stack_print_and_reset(load_es);
    error_stack_destroy(load_es);
    config_destroy(config);
    return;
  }
  error_stack_destroy(load_es);

  ThreadControl *tc = config_get_thread_control(config);
  Game *game = game_duplicate(config_get_game(config));
  const LetterDistribution *ld = game_get_ld(game);

  MoveList *nested_move_list = move_list_create(outer_K);
  SimResults *nested_sim_results = sim_results_create(0.0);
  SimCtx *nested_sim_ctx = NULL;

  FILE *csv = NULL;
  if (csv_path && *csv_path) {
    csv = fopen(csv_path, "w");
    if (csv) {
      fprintf(csv,
              "game,turn,on_turn_player,"
              "inner_calls,inner_rollouts,inner_agree_count,inner_agree_rate,"
              "inner_loss_mean,inner_loss_stddev,inner_loss_max,"
              "inner_early_stops,inner_early_stop_rate,"
              "nested_pick,rack,cgp\n");
      fflush(csv);
    }
  }

  ErrorStack *err = error_stack_create();
  StringBuilder *sb = string_builder_create();

  // Screening mode: read an inneragree CSV, run flat outer sim at each CGP
  // at a modest budget, and emit one CSV row per position with flat's pick,
  // flat's wpct for its pick, and flat's wpct for nested's pick (looked up
  // by move-equivalence). Cheap pass to identify positions where flat and
  // nested most disagree before committing to a full 30-min augment.
  const char *screen_csv_path = getenv("INNERAGREE_SCREEN_CSV");
  if (screen_csv_path && *screen_csv_path) {
    const char *screen_out_path = getenv("INNERAGREE_SCREEN_OUT");
    if (!screen_out_path || !*screen_out_path) {
      printf("error: INNERAGREE_SCREEN_CSV requires INNERAGREE_SCREEN_OUT\n");
    } else {
      MoveList *scr_move_list = move_list_create(outer_K);
      SimResults *scr_sim_results = sim_results_create(0.0);
      SimCtx *scr_sim_ctx = NULL;
      FILE *in = fopen(screen_csv_path, "r");
      FILE *out = fopen(screen_out_path, "w");
      if (!in || !out) {
        printf("error: could not open screen files (%s -> %s)\n",
               screen_csv_path, screen_out_path);
      } else {
        fprintf(out, "game,turn,on_turn_player,nested_pick,"
                     "flat_pick,flat_wpct,flat_wpct_for_nested_pick,cgp\n");
        char *line = NULL;
        size_t cap = 0;
        ssize_t got = getline(&line, &cap, in); // skip header
        (void)got;
        int processed = 0;
        while ((got = getline(&line, &cap, in)) != -1) {
          while (got > 0 && (line[got-1]=='\n' || line[got-1]=='\r')) {
            line[--got] = '\0';
          }
          // Need: game, turn, on_turn_player (first 3), nested_pick (col 13),
          // cgp (last).
          int game_id = 0, turn = 0, on_turn_player = 0;
          char *fields[15] = {0};
          int nf = 0;
          char *save = NULL;
          char *line_copy = string_duplicate(line);
          char *tok = strtok_r(line_copy, ",", &save);
          while (tok && nf < 15) {
            fields[nf++] = tok;
            tok = strtok_r(NULL, ",", &save);
          }
          if (nf < 15) { free(line_copy); continue; }
          game_id = atoi(fields[0]);
          turn = atoi(fields[1]);
          on_turn_player = atoi(fields[2]);
          char *nested_pick_str = string_duplicate(fields[12]);
          char *cgp = string_duplicate(fields[14]);
          free(line_copy);

          ErrorStack *load_err = error_stack_create();
          game_load_cgp(game, cgp, load_err);
          if (!error_stack_is_empty(load_err)) {
            error_stack_print_and_reset(load_err);
            error_stack_destroy(load_err);
            free(nested_pick_str); free(cgp); continue;
          }
          error_stack_destroy(load_err);

          const Move *flat_pick = run_outer_flat(
              game, scr_move_list, scr_sim_results, &scr_sim_ctx, win_pcts, tc,
              outer_K, outer_min_iter, outer_plies, num_threads, tlim,
              /*use_heat_map=*/false, w_winpct, w_spread, spread_scale, err);
          if (!error_stack_is_empty(err)) {
            error_stack_print_and_reset(err);
            free(nested_pick_str); free(cgp); continue;
          }
          string_builder_clear(sb);
          string_builder_add_move(sb, game_get_board(game), flat_pick, ld,
                                  false);
          char *flat_pick_str = string_duplicate(string_builder_peek(sb));

          // Find flat's wpct for its top pick + for nested's pick.
          BAIResult *brk = sim_results_get_bai_result(scr_sim_results);
          int top_arm = bai_result_get_best_arm(brk);
          if (top_arm < 0) top_arm = 0;
          const SimmedPlay *top_sp =
              sim_results_get_simmed_play(scr_sim_results, top_arm);
          const double flat_wpct =
              stat_get_mean(simmed_play_get_win_pct_stat(top_sp));

          double flat_wpct_for_nested = 0.0;
          int n_plays = sim_results_get_number_of_plays(scr_sim_results);
          for (int i = 0; i < n_plays; i++) {
            const SimmedPlay *sp =
                sim_results_get_simmed_play(scr_sim_results, i);
            const Move *m = simmed_play_get_move(sp);
            string_builder_clear(sb);
            string_builder_add_move(sb, game_get_board(game), m, ld, false);
            if (strcmp(string_builder_peek(sb), nested_pick_str) == 0) {
              flat_wpct_for_nested =
                  stat_get_mean(simmed_play_get_win_pct_stat(sp));
              break;
            }
          }

          fprintf(out,
                  "%d,%d,%d,%s,%s,%.6f,%.6f,%s\n",
                  game_id, turn, on_turn_player, nested_pick_str,
                  flat_pick_str, flat_wpct, flat_wpct_for_nested, cgp);
          fflush(out);
          free(nested_pick_str); free(flat_pick_str); free(cgp);
          processed++;
          if (processed % 10 == 0) {
            printf("screened %d positions\n", processed);
          }
        }
        printf("screened %d total → %s\n", processed, screen_out_path);
        free(line);
        fclose(in);
        fclose(out);
      }
      sim_ctx_destroy(scr_sim_ctx);
      sim_results_destroy(scr_sim_results);
      move_list_destroy(scr_move_list);
      string_builder_destroy(sb);
      error_stack_destroy(err);
      sim_ctx_destroy(nested_sim_ctx);
      sim_results_destroy(nested_sim_results);
      move_list_destroy(nested_move_list);
      win_pct_destroy(win_pcts);
      game_destroy(game);
      config_destroy(config);
      return;
    }
  }

  // Nested screening mode: like the flat screening above, but runs a nested
  // outer sim and reports nested's pick, nested's wpct for its pick, and
  // nested's wpct for the flat pick from the input CSV. Input must be the
  // FLAT screening output (has both flat_pick and nested_pick columns).
  const char *nscreen_csv_path = getenv("INNERAGREE_NSCREEN_CSV");
  if (nscreen_csv_path && *nscreen_csv_path) {
    const char *nscreen_out_path = getenv("INNERAGREE_NSCREEN_OUT");
    if (!nscreen_out_path || !*nscreen_out_path) {
      printf("error: INNERAGREE_NSCREEN_CSV requires INNERAGREE_NSCREEN_OUT\n");
    } else {
      MoveList *ns_move_list = move_list_create(outer_K);
      SimResults *ns_sim_results = sim_results_create(0.0);
      SimCtx *ns_sim_ctx = NULL;
      FILE *in = fopen(nscreen_csv_path, "r");
      FILE *out = fopen(nscreen_out_path, "w");
      if (!in || !out) {
        printf("error: could not open nscreen files (%s -> %s)\n",
               nscreen_csv_path, nscreen_out_path);
      } else {
        fprintf(out, "game,turn,on_turn_player,nested_pick_orig,flat_pick,"
                     "nested_top_pick,nested_top_wpct,"
                     "nested_wpct_for_flat_pick,cgp\n");
        char *line = NULL;
        size_t cap = 0;
        ssize_t got = getline(&line, &cap, in); // skip header
        (void)got;
        int processed = 0;
        // The flat-screening output columns are:
        //   game,turn,on_turn_player,nested_pick(orig),flat_pick,
        //   flat_wpct,flat_wpct_for_nested_pick,cgp
        while ((got = getline(&line, &cap, in)) != -1) {
          while (got > 0 && (line[got-1]=='\n' || line[got-1]=='\r')) {
            line[--got] = '\0';
          }
          char *fields[8] = {0};
          int nf = 0;
          char *save = NULL;
          char *line_copy = string_duplicate(line);
          char *tok = strtok_r(line_copy, ",", &save);
          while (tok && nf < 8) {
            fields[nf++] = tok;
            tok = strtok_r(NULL, ",", &save);
          }
          if (nf < 8) { free(line_copy); continue; }
          int game_id = atoi(fields[0]);
          int turn = atoi(fields[1]);
          int on_turn_player = atoi(fields[2]);
          char *nested_pick_orig = string_duplicate(fields[3]);
          char *flat_pick_str = string_duplicate(fields[4]);
          char *cgp = string_duplicate(fields[7]);
          free(line_copy);

          ErrorStack *load_err = error_stack_create();
          game_load_cgp(game, cgp, load_err);
          if (!error_stack_is_empty(load_err)) {
            error_stack_print_and_reset(load_err);
            error_stack_destroy(load_err);
            free(nested_pick_orig); free(flat_pick_str); free(cgp); continue;
          }
          error_stack_destroy(load_err);

          run_outer_nested(game, ns_move_list, ns_sim_results, &ns_sim_ctx,
                           win_pcts, tc, outer_K, outer_min_iter, outer_plies,
                           num_threads, tlim, /*use_heat_map=*/false, inner_K,
                           inner_floor, inner_max, inner_plies, inner_stop_z,
                           w_winpct, w_spread, spread_scale, err);
          if (!error_stack_is_empty(err)) {
            error_stack_print_and_reset(err);
            free(nested_pick_orig); free(flat_pick_str); free(cgp); continue;
          }

          BAIResult *brk = sim_results_get_bai_result(ns_sim_results);
          int top_arm = bai_result_get_best_arm(brk);
          if (top_arm < 0) top_arm = 0;
          const SimmedPlay *top_sp =
              sim_results_get_simmed_play(ns_sim_results, top_arm);
          const double nested_top_wpct =
              stat_get_mean(simmed_play_get_win_pct_stat(top_sp));
          string_builder_clear(sb);
          string_builder_add_move(sb, game_get_board(game),
                                  simmed_play_get_move(top_sp), ld, false);
          char *nested_top_pick_str = string_duplicate(string_builder_peek(sb));

          double nested_wpct_for_flat = 0.0;
          int n_plays = sim_results_get_number_of_plays(ns_sim_results);
          for (int i = 0; i < n_plays; i++) {
            const SimmedPlay *sp =
                sim_results_get_simmed_play(ns_sim_results, i);
            const Move *m = simmed_play_get_move(sp);
            string_builder_clear(sb);
            string_builder_add_move(sb, game_get_board(game), m, ld, false);
            if (strcmp(string_builder_peek(sb), flat_pick_str) == 0) {
              nested_wpct_for_flat =
                  stat_get_mean(simmed_play_get_win_pct_stat(sp));
              break;
            }
          }

          fprintf(out, "%d,%d,%d,%s,%s,%s,%.6f,%.6f,%s\n",
                  game_id, turn, on_turn_player, nested_pick_orig,
                  flat_pick_str, nested_top_pick_str, nested_top_wpct,
                  nested_wpct_for_flat, cgp);
          fflush(out);
          free(nested_pick_orig); free(flat_pick_str); free(cgp);
          free(nested_top_pick_str);
          processed++;
          if (processed % 5 == 0) {
            printf("nscreened %d positions\n", processed);
          }
        }
        printf("nscreened %d total → %s\n", processed, nscreen_out_path);
        free(line);
        fclose(in);
        fclose(out);
      }
      sim_ctx_destroy(ns_sim_ctx);
      sim_results_destroy(ns_sim_results);
      move_list_destroy(ns_move_list);
      string_builder_destroy(sb);
      error_stack_destroy(err);
      sim_ctx_destroy(nested_sim_ctx);
      sim_results_destroy(nested_sim_results);
      move_list_destroy(nested_move_list);
      win_pct_destroy(win_pcts);
      game_destroy(game);
      config_destroy(config);
      return;
    }
  }

  // Dump mode: print a single position via string_builder_add_game and exit.
  const char *dump_cgp = getenv("INNERAGREE_DUMP_CGP");
  if (dump_cgp && *dump_cgp) {
    ErrorStack *load_err = error_stack_create();
    game_load_cgp(game, dump_cgp, load_err);
    if (!error_stack_is_empty(load_err)) {
      error_stack_print_and_reset(load_err);
    } else {
      GameStringOptions *gso = game_string_options_create_default();
      string_builder_clear(sb);
      string_builder_add_game(game, NULL, gso, NULL, sb);
      printf("%s\n", string_builder_peek(sb));
      printf("CGP: %s\n", dump_cgp);
      game_string_options_destroy(gso);
    }
    error_stack_destroy(load_err);
    string_builder_destroy(sb);
    error_stack_destroy(err);
    sim_ctx_destroy(nested_sim_ctx);
    sim_results_destroy(nested_sim_results);
    move_list_destroy(nested_move_list);
    win_pct_destroy(win_pcts);
    game_destroy(game);
    config_destroy(config);
    return;
  }

  // ---- Golden corpus modes ------------------------------------------------
  //
  // GOLDEN BUILD: read positions CSV, run a long-budget nested-only sim per
  // position with disable_similarity=true and no heat-map, emit per-position
  // JSONL with per-arm (move, win_pct, sample_count, per-ply score+bingo).
  // Output is light vs the augment JSONL (no 225-cell heatmaps).
  //
  // Env: INNERAGREE_GOLDEN_BUILD_CSV (input), INNERAGREE_GOLDEN_BUILD_OUT
  // (output JSONL). Per-position budget = INNERAGREE_TLIM seconds.
  const char *gb_csv_path = getenv("INNERAGREE_GOLDEN_BUILD_CSV");
  if (gb_csv_path && *gb_csv_path) {
    const char *gb_out_path = getenv("INNERAGREE_GOLDEN_BUILD_OUT");
    if (!gb_out_path || !*gb_out_path) {
      printf("error: INNERAGREE_GOLDEN_BUILD_CSV requires "
             "INNERAGREE_GOLDEN_BUILD_OUT\n");
    } else {
      MoveList *gb_move_list = move_list_create(outer_K);
      SimResults *gb_sim_results = sim_results_create(0.0);
      SimCtx *gb_sim_ctx = NULL;
      FILE *in = fopen(gb_csv_path, "r");
      FILE *out = fopen(gb_out_path, "w");
      if (!in || !out) {
        printf("error: could not open golden-build files (%s -> %s)\n",
               gb_csv_path, gb_out_path);
      } else {
        char *line = NULL;
        size_t cap = 0;
        ssize_t got = getline(&line, &cap, in); // skip header
        (void)got;
        int processed = 0;
        while ((got = getline(&line, &cap, in)) != -1) {
          while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) {
            line[--got] = '\0';
          }
          int game_id = 0, turn = 0, on_turn_player = 0;
          {
            char *p = line;
            game_id = atoi(p);
            p = strchr(p, ','); if (!p) continue; p++;
            turn = atoi(p);
            p = strchr(p, ','); if (!p) continue; p++;
            on_turn_player = atoi(p);
          }
          char *last_comma = strrchr(line, ',');
          if (!last_comma) {
            continue;
          }
          const char *cgp = last_comma + 1;
          ErrorStack *load_err = error_stack_create();
          game_load_cgp(game, cgp, load_err);
          if (!error_stack_is_empty(load_err)) {
            error_stack_print_and_reset(load_err);
            error_stack_destroy(load_err);
            continue;
          }
          error_stack_destroy(load_err);
          char *cgp_copy = string_duplicate(cgp);

          run_outer_nested(game, gb_move_list, gb_sim_results, &gb_sim_ctx,
                           win_pcts, tc, outer_K, outer_min_iter, outer_plies,
                           num_threads, tlim, /*use_heat_map=*/false, inner_K,
                           inner_floor, inner_max, inner_plies, inner_stop_z,
                           w_winpct, w_spread, spread_scale, err);
          if (!error_stack_is_empty(err)) {
            error_stack_print_and_reset(err);
            free(cgp_copy);
            continue;
          }

          // Re-load CGP so move strings reference the root board.
          ErrorStack *re = error_stack_create();
          game_load_cgp(game, cgp_copy, re);
          error_stack_destroy(re);

          fprintf(out, "{\"game\":%d,\"turn\":%d,\"on_turn_player\":%d,",
                  game_id, turn, on_turn_player);
          fprintf(out, "\"cgp\":\"%s\",\"tlim\":%g,\"arms\":[", cgp_copy, tlim);
          const int num_arms = sim_results_get_number_of_plays(gb_sim_results);
          const int num_plies = sim_results_get_num_plies(gb_sim_results);
          bool first = true;
          for (int arm = 0; arm < num_arms; arm++) {
            SimmedPlay *sp = sim_results_get_simmed_play(gb_sim_results, arm);
            const Stat *wp = simmed_play_get_win_pct_stat(sp);
            const uint64_t n = stat_get_num_samples(wp);
            if (n == 0) {
              continue;
            }
            const Move *m = simmed_play_get_move(sp);
            string_builder_clear(sb);
            string_builder_add_move(sb, game_get_board(game), m, ld, false);
            fprintf(out, "%s{\"move\":\"%s\",\"win_pct\":%.6f,\"n\":%" PRIu64
                         ",\"plies\":[",
                    first ? "" : ",", string_builder_peek(sb),
                    stat_get_mean(wp), n);
            first = false;
            for (int p = 0; p < num_plies; p++) {
              const Stat *ss_ = simmed_play_get_score_stat(sp, p);
              const Stat *bs_ = simmed_play_get_bingo_stat(sp, p);
              fprintf(out,
                      "%s{\"s\":%.4f,\"sn\":%" PRIu64 ",\"b\":%.4f}",
                      p == 0 ? "" : ",", stat_get_mean(ss_),
                      stat_get_num_samples(ss_), stat_get_mean(bs_));
            }
            fprintf(out, "]}");
          }
          fprintf(out, "]}\n");
          fflush(out);
          free(cgp_copy);
          processed++;
          if (processed % 5 == 0) {
            printf("golden_build: %d positions done → %s\n", processed,
                   gb_out_path);
          }
        }
        printf("golden_build: %d positions total → %s\n", processed,
               gb_out_path);
        free(line);
        fclose(in);
        fclose(out);
      }
      sim_ctx_destroy(gb_sim_ctx);
      sim_results_destroy(gb_sim_results);
      move_list_destroy(gb_move_list);
      string_builder_destroy(sb);
      error_stack_destroy(err);
      sim_ctx_destroy(nested_sim_ctx);
      sim_results_destroy(nested_sim_results);
      move_list_destroy(nested_move_list);
      win_pct_destroy(win_pcts);
      game_destroy(game);
      config_destroy(config);
      return;
    }
  }

  // GOLDEN EVAL: read golden JSONL and a strategy spec, run the strategy at
  // each position, look up the strategy's pick in golden's arms, emit per-
  // position CSV with (golden_top_pick, golden_top_wpct, strat_pick,
  // strat_wpct_in_golden, loss_vs_golden, agree).
  //
  // Strategy is selected via INNERAGREE_GOLDEN_EVAL_STRATEGY = "flat" or
  // "nested" (default flat). Time budget = INNERAGREE_TLIM seconds.
  const char *ge_path = getenv("INNERAGREE_GOLDEN_EVAL_GOLDEN");
  if (ge_path && *ge_path) {
    const char *ge_out_path = getenv("INNERAGREE_GOLDEN_EVAL_OUT");
    const char *strat = getenv("INNERAGREE_GOLDEN_EVAL_STRATEGY");
    const bool use_nested = strat && strcmp(strat, "nested") == 0;
    if (!ge_out_path || !*ge_out_path) {
      printf("error: INNERAGREE_GOLDEN_EVAL_GOLDEN requires "
             "INNERAGREE_GOLDEN_EVAL_OUT\n");
    } else {
      MoveList *ge_move_list = move_list_create(outer_K);
      SimResults *ge_sim_results = sim_results_create(0.0);
      SimCtx *ge_sim_ctx = NULL;
      FILE *in = fopen(ge_path, "r");
      FILE *out = fopen(ge_out_path, "w");
      if (!in || !out) {
        printf("error: could not open golden-eval files (%s -> %s)\n",
               ge_path, ge_out_path);
      } else {
        fprintf(out,
                "game,turn,on_turn_player,strategy,tlim,"
                "golden_top_pick,golden_top_wpct,strat_pick,"
                "strat_wpct_in_golden,loss_vs_golden,agree,found_in_golden\n");
        char *line = NULL;
        size_t cap = 0;
        int processed = 0;
        while (getline(&line, &cap, in) != -1) {
          // Parse only the fields we need from each JSONL line.
          int game_id = 0, turn = 0, on_turn_player = 0;
          char *p;
          p = strstr(line, "\"game\":"); if (!p) continue; game_id = atoi(p+7);
          p = strstr(line, "\"turn\":"); if (!p) continue; turn = atoi(p+7);
          p = strstr(line, "\"on_turn_player\":"); if (!p) continue;
          on_turn_player = atoi(p + strlen("\"on_turn_player\":"));
          // Extract CGP string (between "cgp":"...")
          p = strstr(line, "\"cgp\":\""); if (!p) continue;
          p += strlen("\"cgp\":\"");
          char *q = strchr(p, '"'); if (!q) continue;
          *q = '\0';
          char *cgp = string_duplicate(p);
          *q = '"';
          // Find golden's top arm (highest win_pct) by parsing arm objects.
          char *arms_start = strstr(line, "\"arms\":[");
          if (!arms_start) { free(cgp); continue; }
          char *cursor = arms_start;
          double best_wp = -1.0;
          char *best_move = NULL;
          // Build a quick (move, wpct) map for lookup later.
          // We do a simple scan: for each "move":"..." and "win_pct":N pair.
          char *m_p = cursor;
          // Reset
          best_wp = -1.0;
          if (best_move) { free(best_move); best_move = NULL; }
          while ((m_p = strstr(m_p, "\"move\":\"")) != NULL) {
            m_p += strlen("\"move\":\"");
            char *mq = strchr(m_p, '"');
            if (!mq) break;
            *mq = '\0';
            char *this_move = string_duplicate(m_p);
            *mq = '"';
            char *wp_p = strstr(mq, "\"win_pct\":");
            if (!wp_p) { free(this_move); break; }
            wp_p += strlen("\"win_pct\":");
            double this_wp = atof(wp_p);
            if (this_wp > best_wp) {
              best_wp = this_wp;
              if (best_move) free(best_move);
              best_move = this_move;
            } else {
              free(this_move);
            }
            m_p = wp_p;
          }
          if (!best_move) { free(cgp); continue; }

          // Load CGP and run the chosen strategy.
          ErrorStack *load_err = error_stack_create();
          game_load_cgp(game, cgp, load_err);
          if (!error_stack_is_empty(load_err)) {
            error_stack_print_and_reset(load_err);
            error_stack_destroy(load_err);
            free(cgp); free(best_move); continue;
          }
          error_stack_destroy(load_err);

          if (use_nested) {
            run_outer_nested(game, ge_move_list, ge_sim_results, &ge_sim_ctx,
                             win_pcts, tc, outer_K, outer_min_iter, outer_plies,
                             num_threads, tlim, /*use_heat_map=*/false, inner_K,
                             inner_floor, inner_max, inner_plies, inner_stop_z,
                             w_winpct, w_spread, spread_scale, err);
          } else {
            run_outer_flat(game, ge_move_list, ge_sim_results, &ge_sim_ctx,
                           win_pcts, tc, outer_K, outer_min_iter, outer_plies,
                           num_threads, tlim, /*use_heat_map=*/false, w_winpct,
                           w_spread, spread_scale, err);
          }
          if (!error_stack_is_empty(err)) {
            error_stack_print_and_reset(err);
            free(cgp); free(best_move); continue;
          }
          BAIResult *br = sim_results_get_bai_result(ge_sim_results);
          int top_arm = bai_result_get_best_arm(br);
          if (top_arm < 0) top_arm = 0;
          string_builder_clear(sb);
          string_builder_add_move(
              sb, game_get_board(game),
              simmed_play_get_move(
                  sim_results_get_simmed_play(ge_sim_results, top_arm)),
              ld, false);
          char *strat_pick = string_duplicate(string_builder_peek(sb));

          // Look up strat_pick's win_pct inside the golden arms (re-scan).
          double strat_wp_in_golden = -1.0;
          bool found = false;
          m_p = arms_start;
          while ((m_p = strstr(m_p, "\"move\":\"")) != NULL) {
            m_p += strlen("\"move\":\"");
            char *mq = strchr(m_p, '"');
            if (!mq) break;
            *mq = '\0';
            if (strcmp(m_p, strat_pick) == 0) {
              char *wp_p = strstr(mq + 1, "\"win_pct\":");
              if (wp_p) {
                strat_wp_in_golden = atof(wp_p + strlen("\"win_pct\":"));
                found = true;
              }
              *mq = '"';
              break;
            }
            *mq = '"';
            m_p = mq + 1;
          }

          const bool agree = strcmp(best_move, strat_pick) == 0;
          const double loss =
              found ? (best_wp - strat_wp_in_golden) : -1.0;
          fprintf(out,
                  "%d,%d,%d,%s,%g,%s,%.6f,%s,%.6f,%.6f,%d,%d\n",
                  game_id, turn, on_turn_player,
                  use_nested ? "nested" : "flat", tlim, best_move, best_wp,
                  strat_pick, strat_wp_in_golden, loss, agree ? 1 : 0,
                  found ? 1 : 0);
          fflush(out);
          free(cgp);
          free(best_move);
          free(strat_pick);
          processed++;
          if (processed % 10 == 0) {
            printf("golden_eval: %d positions done\n", processed);
          }
        }
        printf("golden_eval: %d positions total → %s\n", processed,
               ge_out_path);
        free(line);
        fclose(in);
        fclose(out);
      }
      sim_ctx_destroy(ge_sim_ctx);
      sim_results_destroy(ge_sim_results);
      move_list_destroy(ge_move_list);
      string_builder_destroy(sb);
      error_stack_destroy(err);
      sim_ctx_destroy(nested_sim_ctx);
      sim_results_destroy(nested_sim_results);
      move_list_destroy(nested_move_list);
      win_pct_destroy(win_pcts);
      game_destroy(game);
      config_destroy(config);
      return;
    }
  }

  // Augment mode: read an existing inneragree CSV (with cgp + game/turn/
  // on_turn_player), re-run BOTH flat and nested outer sims at each CGP with
  // use_heat_map=true, extract per-arm/per-ply (win_pct, score_mean,
  // score_count, heatmap counts), and emit one JSON line per position to
  // INNERAGREE_AUGMENT_OUT.
  const char *augment_csv_path = getenv("INNERAGREE_AUGMENT_CSV");
  if (augment_csv_path && *augment_csv_path) {
    const char *augment_out_path = getenv("INNERAGREE_AUGMENT_OUT");
    if (!augment_out_path || !*augment_out_path) {
      printf("error: INNERAGREE_AUGMENT_CSV requires INNERAGREE_AUGMENT_OUT\n");
    } else {
      MoveList *flat_move_list = move_list_create(outer_K);
      MoveList *aug_nested_move_list = move_list_create(outer_K);
      SimResults *flat_sim_results = sim_results_create(0.0);
      SimResults *aug_nested_sim_results = sim_results_create(0.0);
      SimCtx *flat_sim_ctx = NULL;
      SimCtx *aug_nested_sim_ctx = NULL;
      FILE *in = fopen(augment_csv_path, "r");
      FILE *out = fopen(augment_out_path, "w");
      if (!in || !out) {
        printf("error: could not open augment files (%s -> %s)\n",
               augment_csv_path, augment_out_path);
      } else {
        char *line = NULL;
        size_t cap = 0;
        // Discard CSV header.
        ssize_t got = getline(&line, &cap, in);
        (void)got;
        int processed = 0;
        while ((got = getline(&line, &cap, in)) != -1) {
          while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) {
            line[--got] = '\0';
          }
          // First three CSV columns are game, turn, on_turn_player (ints);
          // the cgp is the LAST column. Capture them.
          int game_id = 0, turn = 0, on_turn_player = 0;
          {
            char *p = line;
            game_id = atoi(p);
            p = strchr(p, ','); if (!p) continue; p++;
            turn = atoi(p);
            p = strchr(p, ','); if (!p) continue; p++;
            on_turn_player = atoi(p);
          }
          char *last_comma = strrchr(line, ',');
          if (!last_comma) {
            continue;
          }
          const char *cgp = last_comma + 1;
          ErrorStack *load_err = error_stack_create();
          game_load_cgp(game, cgp, load_err);
          if (!error_stack_is_empty(load_err)) {
            error_stack_print_and_reset(load_err);
            error_stack_destroy(load_err);
            continue;
          }
          error_stack_destroy(load_err);

          // Persist a copy of the CGP since the loop reuses `line`.
          char *cgp_copy = string_duplicate(cgp);

          // Run nested sim first; it owns its own move list & sim results.
          run_outer_nested(game, aug_nested_move_list, aug_nested_sim_results,
                           &aug_nested_sim_ctx, win_pcts, tc, outer_K,
                           outer_min_iter, outer_plies, num_threads, tlim,
                           /*use_heat_map=*/true, inner_K, inner_floor,
                           inner_max, inner_plies, inner_stop_z, w_winpct,
                           w_spread, spread_scale, err);
          if (!error_stack_is_empty(err)) {
            error_stack_print_and_reset(err);
            free(cgp_copy);
            continue;
          }

          // Reload CGP — the nested sim mutated game state via play_move
          // chains in its rollout machinery; restore the root.
          ErrorStack *reload_err = error_stack_create();
          game_load_cgp(game, cgp_copy, reload_err);
          if (!error_stack_is_empty(reload_err)) {
            error_stack_print_and_reset(reload_err);
            error_stack_destroy(reload_err);
            free(cgp_copy);
            continue;
          }
          error_stack_destroy(reload_err);

          run_outer_flat(game, flat_move_list, flat_sim_results, &flat_sim_ctx,
                         win_pcts, tc, outer_K, outer_min_iter, outer_plies,
                         num_threads, tlim, /*use_heat_map=*/true, w_winpct,
                         w_spread, spread_scale, err);
          if (!error_stack_is_empty(err)) {
            error_stack_print_and_reset(err);
            free(cgp_copy);
            continue;
          }

          // Reload again so move strings reference the root board.
          ErrorStack *reload2_err = error_stack_create();
          game_load_cgp(game, cgp_copy, reload2_err);
          error_stack_destroy(reload2_err);

          // Emit one JSON object per line.
          fprintf(out, "{\"game\":%d,\"turn\":%d,\"on_turn_player\":%d,",
                  game_id, turn, on_turn_player);
          fprintf(out, "\"cgp\":\"%s\",\"variants\":{", cgp_copy);

          for (int variant = 0; variant < 2; variant++) {
            const char *vname = (variant == 0) ? "nested" : "flat";
            SimResults *res =
                (variant == 0) ? aug_nested_sim_results : flat_sim_results;
            const int num_arms = sim_results_get_number_of_plays(res);
            const int num_plies = sim_results_get_num_plies(res);
            fprintf(out, "%s\"%s\":{\"arms\":[",
                    (variant == 0) ? "" : ",", vname);
            bool first_arm = true;
            for (int arm = 0; arm < num_arms; arm++) {
              SimmedPlay *sp = sim_results_get_simmed_play(res, arm);
              const Stat *wp_stat = simmed_play_get_win_pct_stat(sp);
              const uint64_t wp_count = stat_get_num_samples(wp_stat);
              if (wp_count == 0) {
                continue; // Skip arms BAI pruned out before any sample.
              }
              const Move *m = simmed_play_get_move(sp);
              string_builder_clear(sb);
              string_builder_add_move(sb, game_get_board(game), m, ld, false);
              const double win_pct = stat_get_mean(wp_stat);

              fprintf(out, "%s{\"move\":\"%s\",\"win_pct\":%.6f,"
                           "\"win_pct_count\":%" PRIu64 ",\"plies\":[",
                      first_arm ? "" : ",", string_builder_peek(sb), win_pct,
                      wp_count);
              first_arm = false;

              for (int p = 0; p < num_plies; p++) {
                const Stat *score_stat = simmed_play_get_score_stat(sp, p);
                const Stat *bingo_stat = simmed_play_get_bingo_stat(sp, p);
                HeatMap *hm = simmed_play_get_heat_map(sp, p);
                const uint64_t score_n = stat_get_num_samples(score_stat);
                const double score_mean = stat_get_mean(score_stat);
                const double bingo_mean = stat_get_mean(bingo_stat);

                fprintf(out, "%s{\"score_mean\":%.4f,\"score_count\":%" PRIu64
                             ",\"bingo_rate\":%.6f,\"hm\":[",
                        p == 0 ? "" : ",", score_mean, score_n, bingo_mean);
                if (hm) {
                  for (int row = 0; row < BOARD_DIM; row++) {
                    for (int col = 0; col < BOARD_DIM; col++) {
                      const uint64_t c = heat_map_get_count(
                          hm, row, col, HEAT_MAP_TYPE_ALL);
                      fprintf(out, "%s%" PRIu64,
                              (row == 0 && col == 0) ? "" : ",", c);
                    }
                  }
                }
                fprintf(out, "],\"hm_bingo\":[");
                if (hm) {
                  for (int row = 0; row < BOARD_DIM; row++) {
                    for (int col = 0; col < BOARD_DIM; col++) {
                      const uint64_t c = heat_map_get_count(
                          hm, row, col, HEAT_MAP_TYPE_BINGO);
                      fprintf(out, "%s%" PRIu64,
                              (row == 0 && col == 0) ? "" : ",", c);
                    }
                  }
                }
                fprintf(out, "],\"pass_count\":%" PRIu64,
                        simmed_play_get_ply_info_count(
                            sp, p, PLY_INFO_COUNT_PASS));
                fprintf(out, ",\"exch_counts\":[");
                for (int sz = 1; sz <= RACK_SIZE; sz++) {
                  fprintf(out, "%s%" PRIu64, (sz == 1) ? "" : ",",
                          simmed_play_get_ply_info_count(
                              sp, p, PLY_INFO_COUNT_EXCHANGE_1 + (sz - 1)));
                }
                fprintf(out, "]}");
              }
              fprintf(out, "]}");
            }
            fprintf(out, "]}");
          }
          fprintf(out, "}}\n");
          fflush(out);
          free(cgp_copy);
          processed++;
          if (processed % 5 == 0) {
            printf("augmented %d positions (%s)\n", processed,
                   augment_out_path);
          }
        }
        printf("augmented %d positions total → %s\n", processed,
               augment_out_path);
        free(line);
        fclose(in);
        fclose(out);
      }
      sim_ctx_destroy(flat_sim_ctx);
      sim_ctx_destroy(aug_nested_sim_ctx);
      sim_results_destroy(flat_sim_results);
      sim_results_destroy(aug_nested_sim_results);
      move_list_destroy(flat_move_list);
      move_list_destroy(aug_nested_move_list);
    }
    if (csv) {
      fclose(csv);
    }
    string_builder_destroy(sb);
    error_stack_destroy(err);
    sim_ctx_destroy(nested_sim_ctx);
    sim_results_destroy(nested_sim_results);
    move_list_destroy(nested_move_list);
    win_pct_destroy(win_pcts);
    game_destroy(game);
    config_destroy(config);
    return;
  }

  for (int game_idx = 0; game_idx < num_games; game_idx++) {
    game_reset(game);
    const uint64_t game_seed_val = base_seed + (uint64_t)game_idx;
    game_seed(game, game_seed_val);
    draw_starting_racks(game);

    int turn = 0;
    while (!game_over(game)) {
      // Single nested-outer sim: gameplay (picks move) AND instrumentation
      // (every leaf inner-sim call tallies into InnerDiag counters).
      const Move *nested_pick = run_outer_nested(
          game, nested_move_list, nested_sim_results, &nested_sim_ctx, win_pcts,
          tc, outer_K, outer_min_iter, outer_plies, num_threads, tlim,
          /*use_heat_map=*/false, inner_K, inner_floor, inner_max, inner_plies,
          inner_stop_z, w_winpct, w_spread, spread_scale, err);
      if (!error_stack_is_empty(err)) {
        error_stack_print_and_reset(err);
        break;
      }
      Move nested_pick_copy = *nested_pick;

      InnerDiag diag = {0};
      sim_ctx_get_inner_diag(nested_sim_ctx, &diag);

      const double agree_rate =
          diag.calls > 0 ? (double)diag.agree_count / (double)diag.calls : 0.0;
      const double early_stop_rate =
          diag.calls > 0 ? (double)diag.early_stops / (double)diag.calls : 0.0;
      const double mean_loss =
          diag.calls > 0 ? diag.loss_sum / (double)diag.calls : 0.0;
      double loss_stddev = 0.0;
      if (diag.calls > 1) {
        const double mean_sq = diag.loss_sum_sq / (double)diag.calls;
        const double var = mean_sq - mean_loss * mean_loss;
        loss_stddev = var > 0.0 ? sqrt(var) : 0.0;
      }

      const int on_turn_player = game_get_player_on_turn_index(game);
      char *cgp_str = game_get_cgp(game, true);
      string_builder_clear(sb);
      string_builder_add_move(sb, game_get_board(game), &nested_pick_copy, ld,
                              false);
      char *nested_str = string_duplicate(string_builder_peek(sb));
      string_builder_clear(sb);
      string_builder_add_rack(
          sb, player_get_rack(game_get_player(game, on_turn_player)), ld,
          false);
      char *rack_str = string_duplicate(string_builder_peek(sb));

      printf("[g%d t%d p%d] inner: calls=%" PRIu64
             " agree=%.3f loss(mean=%+.4f std=%.4f max=%+.4f)  pick=%s\n",
             game_idx, turn, on_turn_player, diag.calls, agree_rate, mean_loss,
             loss_stddev, diag.loss_max, nested_str);

      if (csv) {
        fprintf(csv,
                "%d,%d,%d,"
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f,"
                "%.6f,%.6f,%.6f,"
                "%" PRIu64 ",%.6f,"
                "%s,%s,%s\n",
                game_idx, turn, on_turn_player, diag.calls, diag.rollouts,
                diag.agree_count, agree_rate, mean_loss, loss_stddev,
                diag.loss_max, diag.early_stops, early_stop_rate, nested_str,
                rack_str, cgp_str);
        fflush(csv);
      }

      free(cgp_str);
      free(nested_str);
      free(rack_str);

      play_move(nested_pick, game, NULL);
      turn++;
    }
    printf("[game %d done in %d turns]\n", game_idx, turn);
  }

  if (csv) {
    fclose(csv);
  }
  string_builder_destroy(sb);
  error_stack_destroy(err);
  sim_ctx_destroy(nested_sim_ctx);
  sim_results_destroy(nested_sim_results);
  move_list_destroy(nested_move_list);
  win_pct_destroy(win_pcts);
  game_destroy(game);
  config_destroy(config);
}
