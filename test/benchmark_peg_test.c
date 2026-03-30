#include "benchmark_peg_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
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
#include "../src/ent/transposition_table.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  const Rack *mover_rack =
      player_get_rack(game_get_player(game, mover_idx));
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
static void eval_move_all_draws(const Game *base_game_empty, const Move *move,
                                int mover_idx,
                                const uint8_t unseen[MAX_ALPHABET_SIZE],
                                int ld_size, int plies,
                                double endgame_time_budget,
                                ThreadControl *tc, double *win_pct_out,
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

    Move move_copy = *move;
    play_move_without_drawing_tiles(&move_copy, g);

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
    total_wins +=
        ((mover_total > 0) ? 1.0 : (mover_total == 0 ? 0.5 : 0.0)) * cnt;
    weight += cnt;

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

  FILE *f = fopen(PEG1_CGPS_FILE, "w");
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
    fprintf(f, "%s\n", cgp);
    free(cgp);
    found++;
  }

  fclose(f);
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
    FILE *f = fopen(PEG1_CGPS_FILE, "r");
    if (!f) {
      printf("Generating positions...\n");
      test_generate_peg1_cgps();
    } else {
      fclose(f);
    }
  }

  Config *config = config_create_or_die("set -s1 score -s2 score");

  // Read CGPs from file.
  FILE *f = fopen(PEG1_CGPS_FILE, "r");
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
  fclose(f);
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
    snprintf(cmd, sizeof(cmd), "cgp %s -lex NWL20", cgps[pos]);
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
    PegSolver *peg_solver = peg_solver_create();
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
    peg_solve(peg_solver, &peg_args, &peg_result, es);
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
                          config_get_thread_control(config), &peg_emp_wp,
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
                          config_get_thread_control(config), &static_emp_wp,
                          &static_emp_spread);
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

    printf(
        "%-4d %-22s %5.1f%% %+7.2f %5.1f%% %+7.2f  %-22s %5.1f%% %+7.2f  "
        "%5d %5.2fs %s\n",
        pos + 1, peg_str, peg_result.best_win_pct * 100.0,
        peg_result.best_expected_spread, peg_emp_wp * 100.0, peg_emp_spread,
        static_str, static_emp_wp * 100.0, static_emp_spread,
        peg_result.candidates_evaluated, peg_time,
        moves_match ? "SAME" : "DIFF");

    free(peg_str);
    free(static_str);
    error_stack_destroy(es);
    peg_solver_destroy(peg_solver);
    move_list_destroy(static_ml);
    game_destroy(base_game_empty);
  }

  printf("\n--- Summary (%d positions) ---\n", num_positions);
  printf("Move agreement:  %d same, %d different (%.0f%% agreement)\n",
         same_move, diff_move,
         100.0 * same_move / (same_move + diff_move));
  printf("Avg empirical W%%: PEG=%.1f%%  Static=%.1f%%\n",
         100.0 * total_peg_wp / num_positions,
         100.0 * total_static_wp / num_positions);
  printf("Avg spread:      PEG=%+.2f  Static=%+.2f\n",
         total_peg_spread / num_positions,
         total_static_spread / num_positions);
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
