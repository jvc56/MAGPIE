#include "benchmark_peg_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/str/letter_distribution_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void play_until_1_in_bag(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 1 &&
         game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    const MoveGenArgs args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    if (move_list->count == 0) {
      break;
    }
    play_move(move_list->moves[0], game, NULL);
  }
}

static char *format_move_str(const Game *game, const Move *move) {
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, ld, false);
  char *str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return str;
}

static char *format_small_move_str(const Game *game, const SmallMove *sm) {
  Move m;
  small_move_to_move(&m, sm, game_get_board(game));
  return format_move_str(game, &m);
}

// Compute unseen tiles (distribution - mover rack - board).
static int bench_compute_unseen(const Game *game, int mover_idx,
                                uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col)) {
        continue;
      }
      MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) {
          unseen[BLANK_MACHINE_LETTER]--;
        }
      } else {
        if (unseen[ml] > 0) {
          unseen[ml]--;
        }
      }
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total += unseen[ml];
  }
  return total;
}

// Set opponent rack to (unseen - {bag_tile}).
static void bench_set_opp_rack(Rack *opp_rack,
                               const uint8_t unseen[MAX_ALPHABET_SIZE],
                               int ld_size, MachineLetter bag_tile) {
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    int cnt = (int)unseen[ml] - (ml == bag_tile ? 1 : 0);
    for (int tile_idx = 0; tile_idx < cnt; tile_idx++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

// Evaluate a move across all draw scenarios using endgame solve.
// Returns empirical win% and average spread from mover's perspective.
// If log is non-NULL, writes one line per scenario.
static void eval_move_all_draws(const Game *base_game_empty, const Move *move,
                                int mover_idx,
                                const uint8_t unseen[MAX_ALPHABET_SIZE],
                                int ld_size, int plies,
                                double endgame_time_budget, ThreadControl *tc,
                                FILE *log, double *win_pct_out,
                                double *spread_out) {
  int opp_idx = 1 - mover_idx;
  double total_spread = 0.0;
  double total_wins = 0.0;
  int weight = 0;

  for (int tile_type = 0; tile_type < ld_size; tile_type++) {
    int cnt = (int)unseen[tile_type];
    if (cnt == 0) {
      continue;
    }

    // Create scenario: duplicate base (empty bag), play move, set racks.
    Game *g = game_duplicate(base_game_empty);
    game_set_endgame_solving_mode(g);
    game_set_backup_mode(g, BACKUP_MODE_OFF);

    Move scenario_move = *move;
    play_move_without_drawing_tiles(&scenario_move, g);

    // Clear any game-end reason. play_move_without_drawing_tiles may
    // falsely set STANDARD (bingo on empty bag) or CONSECUTIVE_ZEROS.
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
    game_set_consecutive_scoreless_turns(g, 0);

    // Set racks: opp gets unseen minus bag_tile.
    Rack *opp_rack = player_get_rack(game_get_player(g, opp_idx));
    bench_set_opp_rack(opp_rack, unseen, ld_size, (MachineLetter)tile_type);
    // Mover draws the bag tile (only if they played tiles, not a pass).
    if (move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      Rack *mover_rack = player_get_rack(game_get_player(g, mover_idx));
      rack_add_letter(mover_rack, (MachineLetter)tile_type);
    }

    int32_t mover_lead =
        equity_to_int(player_get_score(game_get_player(g, mover_idx))) -
        equity_to_int(player_get_score(game_get_player(g, opp_idx)));

    EndgameCtx *eg_ctx = NULL;
    EndgameResults *results = endgame_results_create();
    EndgameArgs ea = {
        .thread_control = tc,
        .game = g,
        .plies = plies,
        .tt_fraction_of_mem = 0.1,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .soft_time_limit = endgame_time_budget,
        .hard_time_limit = endgame_time_budget,
    };

    ErrorStack *es = error_stack_create();
    endgame_solve(&eg_ctx, &ea, results, es);
    if (!error_stack_is_empty(es)) {
      // Endgame solve failed; treat as unknown (use greedy spread of 0).
      error_stack_destroy(es);
      endgame_results_destroy(results);
      endgame_ctx_destroy(eg_ctx);
      game_destroy(g);
      weight += cnt;
      continue;
    }
    error_stack_destroy(es);

    int eg_val = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
    int32_t mover_total = mover_lead - eg_val;

    total_spread += (double)mover_total * cnt;
    double scenario_win;
    if (mover_total > 0) {
      scenario_win = 1.0;
    } else if (mover_total == 0) {
      scenario_win = 0.5;
    } else {
      scenario_win = 0.0;
    }
    total_wins += scenario_win * cnt;
    weight += cnt;

    if (log) {
      const LetterDistribution *ld = game_get_ld(g);
      StringBuilder *tile_sb = string_builder_create();
      string_builder_add_user_visible_letter(tile_sb, ld,
                                             (MachineLetter)tile_type);
      const char *outcome;
      if (mover_total > 0) {
        outcome = "WIN";
      } else if (mover_total == 0) {
        outcome = "TIE";
      } else {
        outcome = "LOSS";
      }
      (void)fprintf(log, "      bag=%s (cnt=%d): final_spread=%+d  %s\n",
                    string_builder_peek(tile_sb), cnt, mover_total, outcome);
      string_builder_destroy(tile_sb);
    }

    endgame_results_destroy(results);
    endgame_ctx_destroy(eg_ctx);
    game_destroy(g);
  }

  *win_pct_out = (weight > 0) ? total_wins / weight : 0.0;
  *spread_out = (weight > 0) ? total_spread / weight : 0.0;
}

// ---------------------------------------------------------------------------
// Position generation: play random games to 1-in-bag
// ---------------------------------------------------------------------------

#define PEG1_CGPS_FILE "/tmp/peg1_cgps.txt"

void test_generate_peg1_cgps(void) {
  int target = 500;
  int max_attempts = 500000;
  Config *config = config_create_or_die("set -s1 score -s2 score");
  // Load a starting position to initialize the game with a lexicon.
  load_and_exec_config_or_die(config,
                              "cgp " EMPTY_CGP_WITHOUT_OPTIONS " -lex NWL20");

  Game *game = config_get_game(config);
  MoveList *ml = move_list_create(20);

  FILE *f = fopen(PEG1_CGPS_FILE, "we");
  assert(f);

  int found = 0;
  for (int attempt_idx = 0; attempt_idx < max_attempts && found < target;
       attempt_idx++) {
    game_reset(game);
    game_seed(game, 42 + (uint64_t)attempt_idx);
    draw_starting_racks(game);
    play_until_1_in_bag(game, ml);

    if (bag_get_letters(game_get_bag(game)) != 1) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }
    const Rack *r0 = player_get_rack(game_get_player(game, 0));
    const Rack *r1 = player_get_rack(game_get_player(game, 1));
    if (rack_is_empty(r0) || rack_is_empty(r1)) {
      continue;
    }

    char *cgp = game_get_cgp(game, true);
    (void)fprintf(f, "%s\n", cgp);
    free(cgp);
    found++;
  }

  (void)fclose(f);
  printf("Generated %d 1-in-bag positions in %s\n", found, PEG1_CGPS_FILE);

  move_list_destroy(ml);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// A/B Benchmark: PEG (2-ply, time-budgeted) vs Static Eval
// ---------------------------------------------------------------------------

void test_benchmark_peg1(void) {
  log_set_level(LOG_FATAL);
  int num_positions = 10;
  double peg_time_budget = 5.0;
  double endgame_time_budget = 0.0;
  int endgame_plies = 2;

  // First generate positions if file doesn't exist.
  {
    FILE *f = fopen(PEG1_CGPS_FILE, "re");
    if (!f) {
      printf("Generating positions...\n");
      test_generate_peg1_cgps();
    } else {
      (void)fclose(f);
    }
  }

  Config *config = config_create_or_die("set -s1 score -s2 score");

  // Read CGPs from file.
  FILE *f = fopen(PEG1_CGPS_FILE, "re");
  assert(f);
  char **cgps = malloc_or_die(num_positions * sizeof(char *));
  int loaded = 0;
  char line[4096];
  while (loaded < num_positions && fgets(line, sizeof(line), f)) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    cgps[loaded] = string_duplicate(line);
    loaded++;
  }
  (void)fclose(f);
  assert(loaded == num_positions);

  printf("\n%-4s %-22s %6s %7s %6s %7s  %-22s %6s %7s  %5s %6s %s\n", "Pos",
         "PEG Best", "EstW%", "EstSpr", "EmpW%", "Spread", "Static Best",
         "EmpW%", "Spread", "#Eval", "Time", "Match");
  printf("---- ---------------------- ------ ------- ------ -------  "
         "---------------------- ------ -------  ----- ------ -----\n");

  int same_move = 0;
  int diff_move = 0;
  int peg_better = 0;
  int static_better = 0;
  int tied_count = 0;
  double total_peg_wp = 0;
  double total_static_wp = 0;
  double total_peg_spread = 0;
  double total_static_spread = 0;
  double total_peg_time = 0;
  int total_candidates_evaluated = 0;

  for (int pos = 0; pos < num_positions; pos++) {
    // Load position.
    char cmd[4096];
    (void)snprintf(cmd, sizeof(cmd), "cgp %s -lex NWL20", cgps[pos]);
    load_and_exec_config_or_die(config, cmd);
    Game *game = config_get_game(config);

    if (bag_get_letters(game_get_bag(game)) != 1) {
      printf("  pos %d: skipping (bag != 1)\n", pos + 1);
      continue;
    }

    int mover_idx = game_get_player_on_turn_index(game);
    const LetterDistribution *ld = game_get_ld(game);
    int ld_size = ld_get_size(ld);

    uint8_t unseen[MAX_ALPHABET_SIZE];
    bench_compute_unseen(game, mover_idx, unseen);

    // Create a base game with empty bag for scenario setup.
    Game *base_game_empty = game_duplicate(game);
    game_set_endgame_solving_mode(base_game_empty);
    game_set_backup_mode(base_game_empty, BACKUP_MODE_OFF);
    {
      Rack saved_rack;
      rack_copy(&saved_rack,
                player_get_rack(game_get_player(base_game_empty, mover_idx)));
      Bag *bag = game_get_bag(base_game_empty);
      for (int ml = 0; ml < ld_size; ml++) {
        while (bag_get_letter(bag, ml) > 0) {
          bag_draw_letter(bag, (MachineLetter)ml, mover_idx);
        }
      }
      rack_copy(player_get_rack(game_get_player(base_game_empty, mover_idx)),
                &saved_rack);
    }

    // --- Static eval: top equity move ---
    MoveList *static_ml = move_list_create(1);
    const Move *static_move = get_top_equity_move(game, 0, static_ml);
    char *static_str = format_move_str(game, static_move);
    Move static_copy = *static_move;

    // --- PEG solve: greedy + 2-ply endgame, 8 threads ---
    PegArgs peg_args = {
        .game = game,
        .thread_control = config_get_thread_control(config),
        .time_budget_seconds = peg_time_budget,
        .num_threads = 8,
        .tt_fraction_of_mem = 0.25,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .num_passes = 2,
        .pass_candidate_limits = {32, 16},
    };

    Timer peg_timer;
    ctimer_start(&peg_timer);

    PegResult peg_result;
    ErrorStack *es = error_stack_create();
    peg_solve(&peg_args, &peg_result, es);
    assert(error_stack_is_empty(es));

    double peg_time = ctimer_elapsed_seconds(&peg_timer);
    total_peg_time += peg_time;

    char *peg_str = format_small_move_str(game, &peg_result.best_move);

    // --- Empirical playout: evaluate both moves with deep endgame ---
    double peg_emp_wp = 0;
    double peg_emp_spread = 0;
    {
      Move peg_move;
      small_move_to_move(&peg_move, &peg_result.best_move,
                         game_get_board(game));
      eval_move_all_draws(base_game_empty, &peg_move, mover_idx, unseen,
                          ld_size, endgame_plies, endgame_time_budget,
                          config_get_thread_control(config), NULL, &peg_emp_wp,
                          &peg_emp_spread);
    }

    bool moves_match = (strcmp(peg_str, static_str) == 0);
    double static_emp_wp = 0;
    double static_emp_spread = 0;
    if (moves_match) {
      static_emp_wp = peg_emp_wp;
      static_emp_spread = peg_emp_spread;
    } else {
      eval_move_all_draws(base_game_empty, &static_copy, mover_idx, unseen,
                          ld_size, endgame_plies, endgame_time_budget,
                          config_get_thread_control(config), NULL,
                          &static_emp_wp, &static_emp_spread);
    }

    // --- Tally ---
    if (moves_match) {
      same_move++;
    } else {
      diff_move++;
    }

    if (peg_emp_spread > static_emp_spread + 0.01) {
      peg_better++;
    } else if (static_emp_spread > peg_emp_spread + 0.01) {
      static_better++;
    } else {
      tied_count++;
    }

    total_peg_wp += peg_emp_wp;
    total_static_wp += static_emp_wp;
    total_peg_spread += peg_emp_spread;
    total_static_spread += static_emp_spread;
    total_candidates_evaluated += peg_result.candidates_evaluated;

    printf("%-4d %-22s %5.1f%% %+7.2f %5.1f%% %+7.2f  %-22s %5.1f%% %+7.2f  "
           "%5d %5.2fs %s\n",
           pos + 1, peg_str, peg_result.best_win_pct * 100.0,
           peg_result.best_expected_spread, peg_emp_wp * 100.0, peg_emp_spread,
           static_str, static_emp_wp * 100.0, static_emp_spread,
           peg_result.candidates_evaluated, peg_time,
           moves_match ? "SAME" : "DIFF");

    free(peg_str);
    free(static_str);
    error_stack_destroy(es);
    move_list_destroy(static_ml);
    game_destroy(base_game_empty);
  }

  printf("\n--- Summary (%d positions) ---\n", num_positions);
  printf("Move agreement:  %d same, %d different (%.0f%% agreement)\n",
         same_move, diff_move, 100.0 * same_move / (same_move + diff_move));
  printf("Avg empirical W%%: PEG=%.1f%%  Static=%.1f%%\n",
         100.0 * total_peg_wp / num_positions,
         100.0 * total_static_wp / num_positions);
  printf("Avg spread:      PEG=%+.2f  Static=%+.2f\n",
         total_peg_spread / num_positions, total_static_spread / num_positions);
  printf("Spread outcomes: PEG better=%d  Static better=%d  Tied=%d\n",
         peg_better, static_better, tied_count);
  printf("Avg candidates:  %.1f per position\n",
         (double)total_candidates_evaluated / num_positions);
  printf("PEG total time:  %.2fs (avg %.2fs/pos)\n", total_peg_time,
         total_peg_time / num_positions);

  for (int pos = 0; pos < num_positions; pos++) {
    free(cgps[pos]);
  }
  free(cgps);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// A/B Benchmark across PEG configs vs static, with detailed disk logging.
// Each move is empirically scored by playing every bag-tile draw to game-end
// (deep endgame solve, time-budgeted) and averaging spread/win%.
// ---------------------------------------------------------------------------

// Per-pass logging callback. user_data is a FILE *.
static void peg_logging_callback(int pass, int num_evaluated,
                                 const SmallMove *top_moves,
                                 const double *top_values,
                                 const double *top_win_pcts, int num_top,
                                 const Game *game, double elapsed,
                                 double stage_seconds, void *user_data) {
  FILE *log = (FILE *)user_data;
  if (!log) {
    return;
  }
  if (pass == 0) {
    (void)fprintf(log, "    Pass 0 (greedy, %.3fs, %d evaluated):\n",
                  stage_seconds, num_evaluated);
  } else {
    (void)fprintf(
        log, "    Pass %d (%d-ply, %.3fs, %d evaluated, %.3fs cumulative):\n",
        pass, pass, stage_seconds, num_evaluated, elapsed);
  }
  const LetterDistribution *ld = game_get_ld(game);
  int mover_idx = game_get_player_on_turn_index(game);
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int idx = 0; idx < num_top; idx++) {
    Move m;
    small_move_to_move(&m, &top_moves[idx], game_get_board(game));
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &m, ld, false);
    int score = (int)small_move_get_score(&top_moves[idx]);
    Rack leave;
    rack_copy(&leave, mover_rack);
    for (int tile_idx = 0; tile_idx < m.tiles_length; tile_idx++) {
      MachineLetter ml = m.tiles[tile_idx];
      if (ml != PLAYED_THROUGH_MARKER) {
        rack_take_letter(&leave,
                         get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
      }
    }
    string_builder_add_formatted_string(sb, " %d  leave=", score);
    string_builder_add_rack(sb, &leave, ld, false);
    (void)fprintf(log, "      %2d. %-32s  win%%=%5.1f%%  spread=%+7.2f\n",
                  idx + 1, string_builder_peek(sb), top_win_pcts[idx] * 100.0,
                  top_values[idx]);
    string_builder_destroy(sb);
  }
}

void test_benchmark_peg1_configs(void) {
  log_set_level(LOG_FATAL);

  // Configs to compare. Format: pass_candidate_limits[0] / [1].
  struct PegBenchConfig {
    const char *name;
    int greedy_to_1ply;
    int oneply_to_2ply;
  };
  const struct PegBenchConfig configs[] = {
      {"4/2", 4, 2},
      {"12/6", 12, 6},
      {"32/16", 32, 16},
  };
  const int num_configs = (int)(sizeof(configs) / sizeof(configs[0]));

  const int num_positions = 10;
  const double endgame_time_budget = 0.5;
  const int endgame_plies = 30;
  const double peg_time_budget = 0.0; // no PEG time cap; configs gate runtime
  const int peg_threads = 8;
  const int log_top_per_pass = 10;

  // Generate fixtures if needed.
  {
    FILE *f = fopen(PEG1_CGPS_FILE, "re");
    if (!f) {
      printf("Generating positions...\n");
      test_generate_peg1_cgps();
    } else {
      (void)fclose(f);
    }
  }

  // -wmp false: PEG calls endgame_solve, whose BEST_SMALL movegen normally
  // disables WMP via prepare_for_movegen (override_kwg != NULL). However the
  // RIT inline-bingo fast path in move_gen.c routes 7-tile bingo placements
  // through update_best_move_or_insert_into_movelist_wmp regardless of WMP
  // state, and that function fatals on MOVE_RECORD_BEST_SMALL. Disabling WMP
  // at the game level avoids the fatal. A separate WMP+BEST_SMALL benchmark
  // (run during peg-solver development) showed WMP does not speed up
  // endgame_solve at any tested depth (3-7 plies, stuck/non-stuck), so this
  // workaround has no measured perf cost. The proper fix (route BEST_SMALL
  // away from the WMP recording function, or have PEG disable WMP only on
  // its endgame solves) is tracked as a follow-up.
  Config *config = config_create_or_die("set -s1 score -s2 score -wmp false");

  FILE *cgp_file = fopen(PEG1_CGPS_FILE, "re");
  assert(cgp_file);
  char **cgps = malloc_or_die(num_positions * sizeof(char *));
  int loaded = 0;
  char line[4096];
  while (loaded < num_positions && fgets(line, sizeof(line), cgp_file)) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    cgps[loaded] = string_duplicate(line);
    loaded++;
  }
  (void)fclose(cgp_file);
  assert(loaded == num_positions);

  // Open log file with timestamp suffix.
  char log_path[256];
  time_t now = time(NULL);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  (void)snprintf(log_path, sizeof(log_path),
                 "/tmp/peg_bench_%04d%02d%02d_%02d%02d%02d.log",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
  FILE *log = fopen(log_path, "we");
  assert(log);
  (void)fprintf(log, "PEG bench: %d positions, configs=", num_positions);
  for (int cfg_idx = 0; cfg_idx < num_configs; cfg_idx++) {
    (void)fprintf(log, "%s%s", cfg_idx == 0 ? "" : ",", configs[cfg_idx].name);
  }
  (void)fprintf(log, ", endgame=%.2fs/scenario plies=%d, peg_threads=%d\n\n",
                endgame_time_budget, endgame_plies, peg_threads);

  // Per-config tallies.
  double cfg_total_time[8] = {0};
  double cfg_total_emp_wp[8] = {0};
  double cfg_total_emp_spread[8] = {0};
  int cfg_match_static[8] = {0};
  int cfg_better_than_static[8] = {0};
  int cfg_worse_than_static[8] = {0};
  double total_static_wp = 0;
  double total_static_spread = 0;

  // Stdout summary header.
  printf("\nPEG configs vs static, %d positions, %.2fs endgame/scenario\n",
         num_positions, endgame_time_budget);
  printf("Detailed log: %s\n\n", log_path);
  printf("%-4s %-22s %6s %7s", "Pos", "Static", "EmpW%", "EmpSpr");
  for (int cfg_idx = 0; cfg_idx < num_configs; cfg_idx++) {
    char hdr[32];
    (void)snprintf(hdr, sizeof(hdr), "PEG %s", configs[cfg_idx].name);
    printf("  %-22s %6s %7s %5s", hdr, "EmpW%", "EmpSpr", "Time");
  }
  printf("\n");

  for (int pos = 0; pos < num_positions; pos++) {
    char cmd[4096];
    (void)snprintf(cmd, sizeof(cmd), "cgp %s -lex NWL20", cgps[pos]);
    load_and_exec_config_or_die(config, cmd);
    Game *game = config_get_game(config);

    if (bag_get_letters(game_get_bag(game)) != 1) {
      printf("  pos %d: skipping (bag != 1)\n", pos + 1);
      continue;
    }

    int mover_idx = game_get_player_on_turn_index(game);
    const LetterDistribution *ld = game_get_ld(game);
    int ld_size = ld_get_size(ld);

    uint8_t unseen[MAX_ALPHABET_SIZE];
    bench_compute_unseen(game, mover_idx, unseen);

    // Build empty-bag base game for scenario eval.
    Game *base_game_empty = game_duplicate(game);
    game_set_endgame_solving_mode(base_game_empty);
    game_set_backup_mode(base_game_empty, BACKUP_MODE_OFF);
    {
      Rack saved_rack;
      rack_copy(&saved_rack,
                player_get_rack(game_get_player(base_game_empty, mover_idx)));
      Bag *bag = game_get_bag(base_game_empty);
      for (int ml = 0; ml < ld_size; ml++) {
        while (bag_get_letter(bag, ml) > 0) {
          bag_draw_letter(bag, (MachineLetter)ml, mover_idx);
        }
      }
      rack_copy(player_get_rack(game_get_player(base_game_empty, mover_idx)),
                &saved_rack);
    }

    // --- Log per-position header ---
    (void)fprintf(log, "=== Position %d ===\n", pos + 1);
    (void)fprintf(log, "CGP: %s\n", cgps[pos]);
    {
      const Rack *r0 = player_get_rack(game_get_player(game, 0));
      const Rack *r1 = player_get_rack(game_get_player(game, 1));
      int s0 = equity_to_int(player_get_score(game_get_player(game, 0)));
      int s1 = equity_to_int(player_get_score(game_get_player(game, 1)));
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, r0, ld, false);
      (void)fprintf(log, "P1 rack=%s score=%d%s\n", string_builder_peek(sb), s0,
                    mover_idx == 0 ? "  (mover)" : "");
      string_builder_clear(sb);
      string_builder_add_rack(sb, r1, ld, false);
      (void)fprintf(log, "P2 rack=%s score=%d%s\n", string_builder_peek(sb), s1,
                    mover_idx == 1 ? "  (mover)" : "");
      string_builder_destroy(sb);
    }

    // --- Static eval baseline ---
    MoveList *static_ml = move_list_create(1);
    const Move *static_move = get_top_equity_move(game, 0, static_ml);
    char *static_str = format_move_str(game, static_move);
    Move static_copy = *static_move;

    (void)fprintf(log, "\n  [Static] best=%s\n", static_str);
    (void)fprintf(log, "    Empirical (per scenario):\n");
    double static_wp = 0;
    double static_sp = 0;
    eval_move_all_draws(base_game_empty, &static_copy, mover_idx, unseen,
                        ld_size, endgame_plies, endgame_time_budget,
                        config_get_thread_control(config), log, &static_wp,
                        &static_sp);
    (void)fprintf(log, "    avg: W%%=%.1f%% spread=%+.2f\n\n",
                  static_wp * 100.0, static_sp);
    total_static_wp += static_wp;
    total_static_spread += static_sp;

    printf("%-4d %-22s %5.1f%% %+7.2f", pos + 1, static_str, static_wp * 100.0,
           static_sp);

    // --- Each PEG config ---
    for (int cfg_idx = 0; cfg_idx < num_configs; cfg_idx++) {
      (void)fprintf(log, "  [PEG %s]\n", configs[cfg_idx].name);

      PegArgs peg_args = {
          .game = game,
          .thread_control = config_get_thread_control(config),
          .time_budget_seconds = peg_time_budget,
          .num_threads = peg_threads,
          .tt_fraction_of_mem = 0.25,
          .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
          .num_passes = 2,
          .pass_candidate_limits = {configs[cfg_idx].greedy_to_1ply,
                                    configs[cfg_idx].oneply_to_2ply},
          .per_pass_callback = peg_logging_callback,
          .per_pass_callback_data = log,
          .per_pass_num_top = log_top_per_pass,
      };

      Timer peg_timer;
      ctimer_start(&peg_timer);

      PegResult peg_result;
      ErrorStack *es = error_stack_create();
      peg_solve(&peg_args, &peg_result, es);
      assert(error_stack_is_empty(es));
      double peg_time = ctimer_elapsed_seconds(&peg_timer);
      cfg_total_time[cfg_idx] += peg_time;

      char *peg_str = format_small_move_str(game, &peg_result.best_move);
      Move peg_move;
      small_move_to_move(&peg_move, &peg_result.best_move,
                         game_get_board(game));

      (void)fprintf(log,
                    "    Best: %s  win%%=%.1f%%  spread=%+.2f  passes=%d  "
                    "candidates=%d  time=%.3fs\n",
                    peg_str, peg_result.best_win_pct * 100.0,
                    peg_result.best_expected_spread,
                    peg_result.passes_completed,
                    peg_result.candidates_evaluated, peg_time);

      bool match_static = (strcmp(peg_str, static_str) == 0);
      double peg_wp;
      double peg_sp;
      if (match_static) {
        peg_wp = static_wp;
        peg_sp = static_sp;
        cfg_match_static[cfg_idx]++;
        (void)fprintf(
            log, "    Empirical: matches static (W%%=%.1f%% spread=%+.2f)\n",
            peg_wp * 100.0, peg_sp);
      } else {
        (void)fprintf(log, "    Empirical (per scenario):\n");
        eval_move_all_draws(base_game_empty, &peg_move, mover_idx, unseen,
                            ld_size, endgame_plies, endgame_time_budget,
                            config_get_thread_control(config), log, &peg_wp,
                            &peg_sp);
        (void)fprintf(log, "    avg: W%%=%.1f%% spread=%+.2f\n", peg_wp * 100.0,
                      peg_sp);
      }

      cfg_total_emp_wp[cfg_idx] += peg_wp;
      cfg_total_emp_spread[cfg_idx] += peg_sp;
      if (peg_sp > static_sp + 0.01) {
        cfg_better_than_static[cfg_idx]++;
      } else if (peg_sp < static_sp - 0.01) {
        cfg_worse_than_static[cfg_idx]++;
      }

      printf("  %-22s %5.1f%% %+7.2f %4.2fs", peg_str, peg_wp * 100.0, peg_sp,
             peg_time);
      (void)fprintf(log, "\n");

      free(peg_str);
      error_stack_destroy(es);
    }
    printf("\n");
    (void)fprintf(log, "\n");
    (void)fflush(log);

    free(static_str);
    move_list_destroy(static_ml);
    game_destroy(base_game_empty);
  }

  // --- Summary (stdout + log) ---
  printf("\n--- Summary (%d positions, %.2fs endgame/scenario) ---\n",
         num_positions, endgame_time_budget);
  (void)fprintf(log, "--- Summary (%d positions, %.2fs endgame/scenario) ---\n",
                num_positions, endgame_time_budget);
  printf("Static:           EmpW%%=%.1f%%  Spread=%+.2f\n",
         100.0 * total_static_wp / num_positions,
         total_static_spread / num_positions);
  (void)fprintf(log, "Static:           EmpW%%=%.1f%%  Spread=%+.2f\n",
                100.0 * total_static_wp / num_positions,
                total_static_spread / num_positions);
  for (int cfg_idx = 0; cfg_idx < num_configs; cfg_idx++) {
    char buf[256];
    (void)snprintf(
        buf, sizeof(buf),
        "PEG %-5s:        EmpW%%=%.1f%%  Spread=%+.2f  "
        "TotalTime=%.2fs (avg %.2fs/pos)  match=%d  better=%d  worse=%d\n",
        configs[cfg_idx].name,
        100.0 * cfg_total_emp_wp[cfg_idx] / num_positions,
        cfg_total_emp_spread[cfg_idx] / num_positions, cfg_total_time[cfg_idx],
        cfg_total_time[cfg_idx] / num_positions, cfg_match_static[cfg_idx],
        cfg_better_than_static[cfg_idx], cfg_worse_than_static[cfg_idx]);
    printf("%s", buf);
    (void)fprintf(log, "%s", buf);
  }
  printf("\nDetailed log: %s\n", log_path);

  (void)fclose(log);
  for (int pos = 0; pos < num_positions; pos++) {
    free(cgps[pos]);
  }
  free(cgps);
  config_destroy(config);
}
