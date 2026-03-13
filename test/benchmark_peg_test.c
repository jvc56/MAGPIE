#include "benchmark_peg_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/thread_control_defs.h"
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
#include "../src/ent/transposition_table.h"
#include "../src/str/equity_string.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "../src/str/endgame_string.h"
#include "../src/str/rack_string.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void exec_config_quiet_peg(Config *config, const char *cmd) {
  (void)fflush(stdout);
  int saved_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
  (void)dup2(devnull, STDOUT_FILENO);
  close(devnull);

  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_load_command(config, cmd, error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);

  (void)fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

static bool play_until_1_in_bag(Game *game, MoveList *move_list) {
  while (true) {
    int bag_tiles = bag_get_letters(game_get_bag(game));
    if (bag_tiles <= 1) {
      if (bag_tiles != 1)
        return false;
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE)
        return false;
      const Rack *r0 = player_get_rack(game_get_player(game, 0));
      const Rack *r1 = player_get_rack(game_get_player(game, 1));
      if (rack_is_empty(r0) || rack_is_empty(r1))
        return false;
      return true;
    }
    const Move *move = get_top_equity_move(game, 0, move_list);
    play_move(move, game, NULL);
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE)
      return false;
  }
}

static char *format_move(const Game *game, const Move *move) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), (Move *)move,
                          game_get_ld(game), false);
  char *str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return str;
}

// Compute unseen tiles from the mover's perspective: full distribution minus
// mover's rack minus board tiles.
static int bench_compute_unseen(const Game *game, int mover_idx,
                                uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++)
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  const Rack *mover_rack =
      player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++)
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col))
        continue;
      MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0)
          unseen[BLANK_MACHINE_LETTER]--;
      } else {
        if (unseen[ml] > 0)
          unseen[ml]--;
      }
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++)
    total += unseen[ml];
  return total;
}

// ---------------------------------------------------------------------------
// PV formatting
// ---------------------------------------------------------------------------

// Format the endgame PV as a compact string showing each move and the
// end-of-game rack adjustment.  Uses the start_game snapshot stored in
// the EndgameResults by the solver.
static char *format_endgame_pv(EndgameResults *results) {
  endgame_results_lock(results, ENDGAME_RESULT_BEST);
  endgame_results_update_display_data(results);
  endgame_results_unlock(results, ENDGAME_RESULT_BEST);

  const PVLine *pv =
      endgame_results_get_pvline(results, ENDGAME_RESULT_DISPLAY);
  if (pv->num_moves == 0)
    return string_duplicate("(no moves)");

  const Game *start = endgame_results_get_start_game(results);
  if (!start)
    return string_duplicate("(no start game)");

  Game *gc = game_duplicate(start);
  const LetterDistribution *ld = game_get_ld(gc);
  StringBuilder *sb = string_builder_create();
  Move move;

  for (int i = 0; i < pv->num_moves; i++) {
    small_move_to_move(&move, &pv->moves[i], game_get_board(gc));
    if (i > 0)
      string_builder_add_string(sb, " -> ");
    string_builder_add_move(sb, game_get_board(gc), &move, ld, true);
    play_move(&move, gc, NULL);

    if (game_get_game_end_reason(gc) != GAME_END_REASON_NONE) {
      // Show the stuck player's remaining rack and point adjustment.
      int stuck_idx = game_get_player_on_turn_index(gc);
      const Rack *stuck_rack =
          player_get_rack(game_get_player(gc, stuck_idx));
      if (!rack_is_empty(stuck_rack)) {
        int bonus = equity_to_int(rack_get_score(ld, stuck_rack)) * 2;
        string_builder_add_string(sb, " (stuck ");
        string_builder_add_rack(sb, stuck_rack, ld, false);
        string_builder_add_formatted_string(sb, " +%d)", bonus);
      }
      break;
    }
  }

  char *str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  game_destroy(gc);
  return str;
}

// ---------------------------------------------------------------------------
// PEG stage ranking tracker
// ---------------------------------------------------------------------------

enum { BENCH_TRACK_LIMIT = 256 };

typedef struct {
  char target_move_str[256];
  int target_rank[PEG_MAX_STAGES];
  bool target_pruned[PEG_MAX_STAGES];
  int num_stages;
  // Stored moves for retroactive rank lookup of PEG's final choice.
  Move stage_moves[PEG_MAX_STAGES][BENCH_TRACK_LIMIT];
  int stage_counts[PEG_MAX_STAGES];
} BenchTracker;

static void bench_track_callback(int pass, int num_evaluated,
                                  const Move *top_moves,
                                  const double *top_values,
                                  const double *top_win_pcts,
                                  const bool *top_pruned,
                                  const bool *top_spread_known,
                                  const double *top_eval_seconds, int num_top,
                                  const Game *game, double elapsed,
                                  double stage_seconds, void *user_data) {
  (void)pass;
  (void)num_evaluated;
  (void)top_values;
  (void)top_win_pcts;
  (void)top_spread_known;
  (void)top_eval_seconds;
  (void)elapsed;
  (void)stage_seconds;

  BenchTracker *t = (BenchTracker *)user_data;
  int s = t->num_stages;
  if (s >= PEG_MAX_STAGES)
    return;

  t->target_rank[s] = -1;
  t->target_pruned[s] = false;
  int count = num_top < BENCH_TRACK_LIMIT ? num_top : BENCH_TRACK_LIMIT;
  t->stage_counts[s] = count;

  for (int i = 0; i < count; i++) {
    move_copy(&t->stage_moves[s][i], &top_moves[i]);
    char *ms = format_move(game, &top_moves[i]);
    if (strcmp(ms, t->target_move_str) == 0) {
      t->target_rank[s] = i + 1;
      t->target_pruned[s] = top_pruned[i];
    }
    free(ms);
  }
  t->num_stages++;
}

// Format a ranking trajectory string like "#4->#2->#1" or "#1->#8->X"
static void format_rank_trail(char *buf, size_t buf_size, const int *ranks,
                              const bool *pruned, int num_stages) {
  size_t pos = 0;
  for (int s = 0; s < num_stages && pos < buf_size - 1; s++) {
    if (s > 0 && pos < buf_size - 3) {
      buf[pos++] = '-';
      buf[pos++] = '>';
    }
    if (ranks[s] < 0)
      pos += (size_t)snprintf(buf + pos, buf_size - pos, "X");
    else if (pruned && pruned[s])
      pos += (size_t)snprintf(buf + pos, buf_size - pos, "#%d*", ranks[s]);
    else
      pos += (size_t)snprintf(buf + pos, buf_size - pos, "#%d", ranks[s]);
  }
  if (pos < buf_size)
    buf[pos] = '\0';
}

// Find rank of a move (by formatted string) in stored stage data.
static int find_move_rank_in_stage(const BenchTracker *t, int stage,
                                   const Game *game, const char *move_str) {
  for (int i = 0; i < t->stage_counts[stage]; i++) {
    char *ms = format_move(game, &t->stage_moves[stage][i]);
    int match = (strcmp(ms, move_str) == 0);
    free(ms);
    if (match)
      return i + 1;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Endgame playout for a single draw scenario
// ---------------------------------------------------------------------------

// Given a 1-PEG position (1 tile in bag), a chosen move, and a specific
// draw tile, set up the resulting endgame position and solve it.
// Returns the mover's final spread.
//
// Steps:
//   1. Duplicate game, drain the bag (PEG uses drained-bag positions)
//   2. Play the chosen move on the copy
//   3. Assign racks: mover gets bag_tile added, opp gets unseen - bag_tile
//   4. Endgame solve with time budget
//   5. Return mover's total spread = mover_lead - endgame_val
static int solve_scenario(const Game *base_game, const Move *chosen_move,
                          int mover_idx, int opp_idx,
                          const uint8_t unseen[MAX_ALPHABET_SIZE],
                          int ld_size, MachineLetter bag_tile,
                          double soft_time, double hard_time,
                          ThreadControl *tc, EndgameSolver *solver,
                          EndgameResults *results,
                          TranspositionTable *shared_tt) {
  // Replicate peg.c setup_endgame_scenario: duplicate, set endgame mode,
  // play the move on a drained-bag game, fix false game-end, set racks.
  Game *g = game_duplicate(base_game);
  game_set_endgame_solving_mode(g);
  game_set_backup_mode(g, BACKUP_MODE_OFF);

  // Drain the bag so play_move doesn't draw from it.
  {
    Bag *bag = game_get_bag(g);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(bag, ml) > 0)
        bag_draw_letter(bag, (MachineLetter)ml, mover_idx);
    }
  }

  // Play the chosen move.
  Move m;
  move_copy(&m, chosen_move);
  play_move(&m, g, NULL);
  if (game_get_game_end_reason(g) == GAME_END_REASON_STANDARD) {
    Equity bonus = calculate_end_rack_points(
        player_get_rack(game_get_player(g, opp_idx)), game_get_ld(g));
    player_add_to_score(game_get_player(g, mover_idx), -bonus);
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
  }

  // Set racks for this scenario.
  Rack *opp_rack = player_get_rack(game_get_player(g, opp_idx));
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    int cnt = (int)unseen[ml] - (ml == bag_tile ? 1 : 0);
    for (int k = 0; k < cnt; k++)
      rack_add_letter(opp_rack, (MachineLetter)ml);
  }
  Rack *mover_rack = player_get_rack(game_get_player(g, mover_idx));
  rack_add_letter(mover_rack, bag_tile);

  // Compute mover's current lead.
  int32_t mover_lead =
      equity_to_int(player_get_score(game_get_player(g, mover_idx))) -
      equity_to_int(player_get_score(game_get_player(g, opp_idx)));

  // Endgame solve with shared TT.
  EndgameArgs ea = {
      .thread_control = tc,
      .game = g,
      .plies = MAX_SEARCH_DEPTH,
      .shared_tt = shared_tt,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 8,
      .use_heuristics = true,
      .num_top_moves = 1,
      .soft_time_limit = soft_time,
      .hard_time_limit = hard_time,
      .skip_word_pruning = true,
  };
  ErrorStack *es = error_stack_create();
  endgame_solve(solver, &ea, results, es);
  assert(error_stack_is_empty(es));
  error_stack_destroy(es);

  // endgame_val is from the on-turn player's perspective (opp, since mover
  // just played). mover_total = mover_lead - endgame_val.
  int endgame_val = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
  int mover_total = mover_lead - endgame_val;

  game_destroy(g);
  return mover_total;
}

// Evaluate a chosen move across all possible draw scenarios.
// Returns empirical win% and weighted average spread.
// If label is non-NULL, prints per-draw results as they complete.
// NOTE: Only valid for scoring moves. Pass doesn't draw from the bag,
// so the resulting position is another 1-PEG, not an endgame.
// For pass, returns false and leaves outputs unchanged.
static bool eval_move_all_draws(const Game *game, const Move *chosen_move,
                                int mover_idx,
                                const uint8_t unseen[MAX_ALPHABET_SIZE],
                                int ld_size, double soft_time,
                                double hard_time, ThreadControl *tc,
                                TranspositionTable *shared_tt,
                                const char *label,
                                double *win_pct_out, double *spread_out) {
  if (move_get_type(chosen_move) == GAME_EVENT_PASS) {
    if (label)
      printf("      %s: skipped (pass doesn't create an endgame)\n", label);
    return false;
  }

  int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  double total_spread = 0.0;
  double total_wins = 0.0;
  int total_weight = 0;

  // Reuse solver and results across scenarios for the same move.
  EndgameSolver *solver = endgame_solver_create();
  EndgameResults *results = endgame_results_create();

  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;
    int spread = solve_scenario(game, chosen_move, mover_idx, opp_idx, unseen,
                                ld_size, (MachineLetter)t, soft_time,
                                hard_time, tc, solver, results, shared_tt);
    total_spread += (double)spread * cnt;
    total_wins += ((spread > 0) ? 1.0 : (spread == 0 ? 0.5 : 0.0)) * cnt;
    total_weight += cnt;

    if (label) {
      char *tile_str = ld_ml_to_hl(ld, (MachineLetter)t);
      char *pv_str = format_endgame_pv(results);
      printf("      %s draw %s (x%d): spread=%+d %s\n"
             "        PV: %s\n",
             label, tile_str, cnt, spread,
             spread > 0 ? "WIN" : (spread == 0 ? "TIE" : "LOSS"), pv_str);
      free(pv_str);
      free(tile_str);
      (void)fflush(stdout);
    }
  }

  endgame_results_destroy(results);
  endgame_solver_destroy(solver);

  *win_pct_out = (total_weight > 0) ? total_wins / total_weight : 0.0;
  *spread_out = (total_weight > 0) ? total_spread / total_weight : 0.0;
  return true;
}

// ---------------------------------------------------------------------------
// Position generation
// ---------------------------------------------------------------------------

void test_generate_peg1_cgps(void) {
  log_set_level(LOG_FATAL);

  Config *config = config_create_or_die(
      "set -lex NWL20 -threads 1 -s1 score -s2 score -r1 small -r2 small");
  MoveList *move_list = move_list_create(1);
  exec_config_quiet_peg(config, "new");
  Game *game = config_get_game(config);

  const int target = 500;
  const uint64_t base_seed = 42424242;
  const int max_attempts = 500000;

  FILE *fp = fopen("/tmp/peg1_cgps.txt", "we");
  assert(fp);
  int found = 0;

  for (int i = 0; found < target && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);
    if (!play_until_1_in_bag(game, move_list))
      continue;
    char *cgp = game_get_cgp(game, true);
    (void)fprintf(fp, "%s\n", cgp);
    free(cgp);
    found++;
  }

  (void)fclose(fp);
  printf("\n");
  printf("==============================================================\n");
  printf("  Generate 1-PEG CGPs (seed=%llu)\n",
         (unsigned long long)base_seed);
  printf("==============================================================\n");
  printf("  Found: %d positions -> /tmp/peg1_cgps.txt\n", found);
  printf("==============================================================\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// PEG vs Static Eval benchmark
// ---------------------------------------------------------------------------

typedef struct {
  const char *label;
  int num_stages;
  int stage_limits[PEG_MAX_STAGES];
  int num_limits;
  peg_first_win_mode_t first_win_mode;
  bool first_win_spread_all_final;
  double tt_fraction;
  bool early_cutoff;
  // Optional callback for stage tracking (NULL to disable).
  PegPerPassCallback per_pass_callback;
  void *per_pass_callback_data;
  int per_pass_num_top;
} PegBenchConfig;

static PegResult run_peg_config(Config *config, Game *game,
                                const PegBenchConfig *pc, double *elapsed_out) {
  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 8,
      .tt_fraction_of_mem = pc->tt_fraction,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_stages = pc->num_stages,
      .early_cutoff = pc->early_cutoff,
      .first_win_mode = pc->first_win_mode,
      .first_win_spread_all_final = pc->first_win_spread_all_final,
      .per_pass_callback = pc->per_pass_callback,
      .per_pass_callback_data = pc->per_pass_callback_data,
      .per_pass_num_top = pc->per_pass_num_top,
  };
  for (int i = 0; i < pc->num_limits && i < PEG_MAX_STAGES; i++)
    args.stage_candidate_limits[i] = pc->stage_limits[i];

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  Timer timer;
  ctimer_start(&timer);
  peg_solve(solver, &args, &result, error_stack);
  ctimer_stop(&timer);
  if (elapsed_out)
    *elapsed_out = ctimer_elapsed_seconds(&timer);
  assert(error_stack_is_empty(error_stack));
  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  return result;
}

static void run_peg1_ab_benchmark(const char *cgp_file,
                                  const PegBenchConfig *peg_config,
                                  double playout_soft_time,
                                  double playout_hard_time,
                                  int max_positions) {
  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("No CGP file found at %s - run genpeg1 first.\n", cgp_file);
    return;
  }

  Config *config = config_create_or_die(
      "set -lex NWL20 -threads 8 -s1 score -s2 score -r1 small -r2 small");
  exec_config_quiet_peg(config, "new");
  Game *game = config_get_game(config);
  MoveList *static_ml = move_list_create(1);

  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int num_cgps = 0;
  while (num_cgps < max_positions && fgets(cgp_lines[num_cgps], 4096, fp)) {
    size_t len = strlen(cgp_lines[num_cgps]);
    if (len > 0 && cgp_lines[num_cgps][len - 1] == '\n')
      cgp_lines[num_cgps][len - 1] = '\0';
    if (strlen(cgp_lines[num_cgps]) > 0)
      num_cgps++;
  }
  (void)fclose(fp);

  printf("\n");
  printf("================================================================"
         "======================================\n");
  printf("  1-PEG Benchmark: PEG vs Static Eval | %d positions\n", num_cgps);
  printf("  PEG: %s (%d stages)\n", peg_config->label,
         peg_config->num_stages);
  printf("  Playout: endgame solve soft=%.1fs hard=%.1fs per scenario\n",
         playout_soft_time, playout_hard_time);
  printf("================================================================"
         "======================================\n");
  printf("  %4s  %-16s %6s %7s %6s %7s  %-16s %6s %7s  %8s  %s\n", "Pos",
         "PEG Best", "EstW%", "EstSpr", "EmpW%", "Spread", "Static Best",
         "EmpW%", "Spread", "PEG Time", "Match");
  printf("  ----  %-16s ------ ------- ------ -------  %-16s ------ "
         "-------  --------  -----\n",
         "----------------", "----------------");

  double total_peg_time = 0;
  int same_move = 0;
  int diff_move = 0;
  double total_peg_emp_wp = 0;
  double total_static_emp_wp = 0;
  double total_peg_spread = 0;
  double total_static_spread = 0;
  int peg_spread_better = 0;
  int static_spread_better = 0;
  int spread_tied = 0;
  int solved = 0;

  // Shared transposition table for all endgame solves (0.5 of system memory).
  TranspositionTable *shared_tt = transposition_table_create(0.5);

  for (int ci = 0; ci < num_cgps; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    if (!error_stack_is_empty(err)) {
      error_stack_destroy(err);
      continue;
    }
    error_stack_destroy(err);

    if (bag_get_letters(game_get_bag(game)) != 1)
      continue;

    int mover_idx = game_get_player_on_turn_index(game);
    int ld_size = ld_get_size(game_get_ld(game));

    // Show the position.
    printf("  --- Pos %d ---\n", ci + 1);
    {
      StringBuilder *board_sb = string_builder_create();
      string_builder_add_game(game, NULL, NULL, NULL, board_sb);
      char *board_str = string_builder_dump(board_sb, NULL);
      printf("%s", board_str);
      free(board_str);
      string_builder_destroy(board_sb);
    }
    (void)fflush(stdout);

    // Compute unseen tiles.
    uint8_t unseen[MAX_ALPHABET_SIZE];
    bench_compute_unseen(game, mover_idx, unseen);

    // --- Static eval: top equity move (do first) ---
    const Move *static_move = get_top_equity_move(game, 0, static_ml);
    Move static_move_copy;
    move_copy(&static_move_copy, static_move);
    char *static_move_str = format_move(game, &static_move_copy);

    // Reload for PEG solve.
    err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);

    // --- PEG solve with stage tracking callback ---
    BenchTracker tracker = {0};
    snprintf(tracker.target_move_str, sizeof(tracker.target_move_str), "%s",
             static_move_str);

    PegBenchConfig peg_with_cb = *peg_config;
    peg_with_cb.per_pass_callback = bench_track_callback;
    peg_with_cb.per_pass_callback_data = &tracker;
    peg_with_cb.per_pass_num_top = BENCH_TRACK_LIMIT;

    double peg_time;
    PegResult peg_res = run_peg_config(config, game, &peg_with_cb, &peg_time);
    double peg_est_wp = peg_res.best_win_pct;
    double peg_est_spr = peg_res.spread_known ? peg_res.best_expected_spread
                                              : 0.0;

    // Reload for formatting and playout.
    err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);

    char *peg_move_str = format_move(game, &peg_res.best_move);
    bool moves_match = (strcmp(peg_move_str, static_move_str) == 0);

    // Build ranking trails.
    int peg_ranks[PEG_MAX_STAGES];
    for (int s = 0; s < tracker.num_stages; s++)
      peg_ranks[s] =
          find_move_rank_in_stage(&tracker, s, game, peg_move_str);

    char peg_trail[128], static_trail[128];
    format_rank_trail(peg_trail, sizeof(peg_trail), peg_ranks, NULL,
                      tracker.num_stages);
    format_rank_trail(static_trail, sizeof(static_trail), tracker.target_rank,
                      tracker.target_pruned, tracker.num_stages);

    // Show moves with ranking trails.
    printf(
        "    PEG:    %-16s (est W%%=%.1f%%, est spr=%+.1f, time=%.3fs)  %s\n",
        peg_move_str, peg_est_wp * 100.0, peg_est_spr, peg_time, peg_trail);
    printf("    Static: %-16s %s  %s\n", static_move_str,
           moves_match ? "(same)" : "(DIFF)", static_trail);
    (void)fflush(stdout);

    // --- Empirical playout for static eval's move (first) ---
    double static_emp_wp = 0.0, static_emp_spread = 0.0;
    eval_move_all_draws(game, &static_move_copy, mover_idx, unseen, ld_size,
                        playout_soft_time, playout_hard_time,
                        config_get_thread_control(config), shared_tt, "Static",
                        &static_emp_wp, &static_emp_spread);

    // --- Empirical playout for PEG's move ---
    double peg_emp_wp = peg_est_wp, peg_emp_spread = peg_est_spr;
    if (moves_match) {
      peg_emp_wp = static_emp_wp;
      peg_emp_spread = static_emp_spread;
    } else {
      eval_move_all_draws(game, &peg_res.best_move, mover_idx, unseen, ld_size,
                          playout_soft_time, playout_hard_time,
                          config_get_thread_control(config), shared_tt, "PEG",
                          &peg_emp_wp, &peg_emp_spread);
    }

    if (moves_match)
      same_move++;
    else
      diff_move++;

    total_peg_emp_wp += peg_emp_wp;
    total_static_emp_wp += static_emp_wp;
    total_peg_spread += peg_emp_spread;
    total_static_spread += static_emp_spread;
    if (peg_emp_spread > static_emp_spread + 0.01)
      peg_spread_better++;
    else if (static_emp_spread > peg_emp_spread + 0.01)
      static_spread_better++;
    else
      spread_tied++;
    total_peg_time += peg_time;
    solved++;

    printf("  %4d  %-16s %5.1f%% %+6.1f %5.1f%% %+6.1f  %-16s %5.1f%% "
           "%+6.1f  %7.3fs  %s\n",
           ci + 1, peg_move_str, peg_est_wp * 100.0, peg_est_spr,
           peg_emp_wp * 100.0, peg_emp_spread, static_move_str,
           static_emp_wp * 100.0, static_emp_spread, peg_time,
           moves_match ? "same" : "DIFF");

    free(peg_move_str);
    free(static_move_str);

    if ((ci + 1) % 10 == 0)
      (void)fflush(stdout);
  }

  printf("  ----  %-16s ------ ------- ------ -------  %-16s ------ "
         "-------  --------  -----\n",
         "----------------", "----------------");
  printf("\n");
  printf("  Results (%d positions):\n", solved);
  printf("    Same best move: %d  |  Different: %d  (%.1f%% agreement)\n",
         same_move, diff_move,
         solved > 0 ? 100.0 * same_move / solved : 0.0);
  printf("    PEG avg empirical win%%:    %.2f%%\n",
         solved > 0 ? 100.0 * total_peg_emp_wp / solved : 0.0);
  printf("    Static avg empirical win%%: %.2f%%\n",
         solved > 0 ? 100.0 * total_static_emp_wp / solved : 0.0);
  printf("    PEG avg game spread:    %+.2f\n",
         solved > 0 ? total_peg_spread / solved : 0.0);
  printf("    Static avg game spread: %+.2f\n",
         solved > 0 ? total_static_spread / solved : 0.0);
  printf("    PEG spread better: %d  |  Static better: %d  |  Tied: %d\n",
         peg_spread_better, static_spread_better, spread_tied);
  printf("    PEG total solve time: %.3fs (avg %.3fs)\n", total_peg_time,
         solved > 0 ? total_peg_time / solved : 0.0);
  printf("================================================================"
         "======================================\n");
  (void)fflush(stdout);

  transposition_table_destroy(shared_tt);
  move_list_destroy(static_ml);
  free(cgp_lines);
  config_destroy(config);
}


// ---------------------------------------------------------------------------
// Debug test for position 3
// ---------------------------------------------------------------------------

void test_peg3_debug(void) {
  log_set_level(LOG_WARN);

  const char *cgp =
      "15/4B3C1V4/2J1IN2O1E1E2/2A1TE1ADAXIAL1/"
      "2I1EW2e1T1R2/2L2C1TI3P2/1WOO1O1ZA3L2/"
      "T1RHUMBAS2VUGS/A4E1r4G2/R2Q1R1I7/I2U3S7/"
      "F2O3T7/FONTINAS7/E2H5YEUK2/D2ADENINE5 "
      "EEGLNRR/EIMOOPY 384/397 0";

  Config *config = config_create_or_die(
      "set -lex NWL20 -threads 8 -s1 score -s2 score -r1 small -r2 small");
  exec_config_quiet_peg(config, "new");
  Game *game = config_get_game(config);

  ErrorStack *err = error_stack_create();
  game_load_cgp(game, cgp, err);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  // Print the full board.
  printf("\n");
  StringBuilder *sb = string_builder_create();
  string_builder_add_game(game, NULL, NULL, NULL, sb);
  char *game_str = string_builder_dump(sb, NULL);
  printf("%s", game_str);
  free(game_str);
  string_builder_destroy(sb);

  int mover_idx = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  // Compute unseen tiles.
  uint8_t unseen[MAX_ALPHABET_SIZE];
  int total_unseen = bench_compute_unseen(game, mover_idx, unseen);
  printf("Unseen: %d tiles, Bag: %d\n", total_unseen,
         bag_get_letters(game_get_bag(game)));
  printf("Unseen tiles:");
  for (int t = 0; t < ld_size; t++) {
    if (unseen[t] == 0)
      continue;
    char *ts = ld_ml_to_hl(ld, (MachineLetter)t);
    printf(" %s(%d)", ts, unseen[t]);
    free(ts);
  }
  printf("\n");

  // Generate all moves and show top by equity.
  MoveList *ml = move_list_create(512);
  {
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .thread_index = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  move_list_sort_moves(ml);
  int show = ml->count < 20 ? ml->count : 20;
  printf("\nTop %d of %d moves by equity:\n", show, ml->count);
  for (int i = 0; i < show; i++) {
    const Move *mov = move_list_get_move(ml, i);
    char *ms = format_move(game, mov);
    StringBuilder *eq_sb = string_builder_create();
    string_builder_add_equity(eq_sb, move_get_equity(mov), "%+.2f");
    char *eq_str = string_builder_dump(eq_sb, NULL);
    printf("  %2d. %-24s  equity=%s\n", i + 1, ms, eq_str);
    free(eq_str);
    string_builder_destroy(eq_sb);
    free(ms);
  }

  // Find pass and target move (containing "REGL").
  Move pass_move, target_move;
  bool found_pass = false, found_target = false;
  int target_rank = -1;
  for (int i = 0; i < ml->count; i++) {
    const Move *mov = move_list_get_move(ml, i);
    if (move_get_type(mov) == GAME_EVENT_PASS && !found_pass) {
      move_copy(&pass_move, mov);
      found_pass = true;
      continue;
    }
    if (!found_target) {
      char *ms = format_move(game, mov);
      if (strstr(ms, "REGL")) {
        move_copy(&target_move, mov);
        found_target = true;
        target_rank = i + 1;
        StringBuilder *eq_sb = string_builder_create();
        string_builder_add_equity(eq_sb, move_get_equity(mov), "%+.2f");
        char *eq_str = string_builder_dump(eq_sb, NULL);
        printf("\nTarget move: %s (rank %d, equity=%s)\n", ms, target_rank,
               eq_str);
        free(eq_str);
        string_builder_destroy(eq_sb);
      }
      free(ms);
    }
  }
  move_list_destroy(ml);

  if (!found_pass) {
    printf("ERROR: pass not found\n");
    config_destroy(config);
    return;
  }
  if (!found_target) {
    printf("WARNING: target move containing 'REGL' not found\n");
  }

  // PEG solve.
  printf("\n--- PEG Solve ---\n");
  (void)fflush(stdout);
  err = error_stack_create();
  game_load_cgp(game, cgp, err);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  PegBenchConfig peg = {
      .label = "debug",
      .num_stages = 3,
      .stage_limits = {16, 8},
      .num_limits = 2,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
      .first_win_spread_all_final = true,
      .tt_fraction = 0.5,
      .early_cutoff = true,
  };
  double peg_time;
  PegResult peg_res = run_peg_config(config, game, &peg, &peg_time);

  err = error_stack_create();
  game_load_cgp(game, cgp, err);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  char *peg_move_str = format_move(game, &peg_res.best_move);
  printf("PEG best: %s (win%%=%.1f%%, est_spr=%+.1f, time=%.3fs)\n",
         peg_move_str, peg_res.best_win_pct * 100.0,
         peg_res.spread_known ? peg_res.best_expected_spread : 0.0, peg_time);
  free(peg_move_str);

  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Public test entries
// ---------------------------------------------------------------------------

void test_benchmark_peg1(void) {
  log_set_level(LOG_FATAL);

  PegBenchConfig peg = {
      .label = "4-stg {200,20,16}",
      .num_stages = 4,
      .stage_limits = {200, 20, 16},
      .num_limits = 3,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
      .first_win_spread_all_final = true,
      .tt_fraction = 0.5,
      .early_cutoff = true,
  };

  // Endgame playout time budgets per scenario.
  double soft_time = 2.0;
  double hard_time = 2.0;

  run_peg1_ab_benchmark("/tmp/peg1_cgps.txt", &peg, soft_time, hard_time, 500);
}

void test_benchmark_peg1_wide(void) {
  log_set_level(LOG_FATAL);

  PegBenchConfig peg = {
      .label = "4-stg wide {256,16,4}",
      .num_stages = 4,
      .stage_limits = {256, 16, 4},
      .num_limits = 3,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
      .first_win_spread_all_final = true,
      .tt_fraction = 0.5,
      .early_cutoff = true,
  };

  double soft_time = 2.0;
  double hard_time = 2.0;

  run_peg1_ab_benchmark("/tmp/peg1_static_better_5.txt", &peg, soft_time,
                        hard_time, 5);
}

// Format a PVLine into a string builder with move notation and end-of-game
// annotations. '|' separates negamax-proven moves from greedy extension.
static void format_pvline_bench(StringBuilder *sb, const PVLine *pv_line,
                                const Game *game) {
  const LetterDistribution *ld = game_get_ld(game);
  Move move;
  Game *gc = game_duplicate(game);
  for (int i = 0; i < pv_line->num_moves; i++) {
    small_move_to_move(&move, &(pv_line->moves[i]), game_get_board(gc));
    string_builder_add_move(sb, game_get_board(gc), &move, ld, true);
    play_move(&move, gc, NULL);
    if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
      int opp_idx = game_get_player_on_turn_index(gc);
      const Rack *opp_rack = player_get_rack(game_get_player(gc, opp_idx));
      int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
      string_builder_add_string(sb, " (");
      string_builder_add_rack(sb, opp_rack, ld, false);
      string_builder_add_formatted_string(sb, " +%d)", adj);
    }
    if (i < pv_line->num_moves - 1) {
      if (i + 1 == pv_line->negamax_depth && pv_line->negamax_depth > 0) {
        string_builder_add_string(sb, " |");
      }
      string_builder_add_string(sb, " ");
    }
  }
  game_destroy(gc);
}

static void append_outcome_bench(StringBuilder *sb, const Game *game,
                                 int32_t spread_delta) {
  int on_turn = game_get_player_on_turn_index(game);
  int p1 = equity_to_int(player_get_score(game_get_player(game, 0)));
  int p2 = equity_to_int(player_get_score(game_get_player(game, 1)));
  int final_spread =
      (p1 - p2) + (on_turn == 0 ? spread_delta : -spread_delta);
  if (final_spread > 0) {
    string_builder_add_formatted_string(sb, " [P1 wins by %d]", final_spread);
  } else if (final_spread < 0) {
    string_builder_add_formatted_string(sb, " [P2 wins by %d]", -final_spread);
  } else {
    string_builder_add_string(sb, " [Tie]");
  }
}

// Per-ply callback that prints the PV and all ranked root moves.
static void pos6_per_ply_callback(int depth, int32_t value,
                                  const PVLine *pv_line, const Game *game,
                                  const PVLine *ranked_pvs,
                                  int num_ranked_pvs, void *user_data) {
  const Timer *timer = (const Timer *)user_data;
  double elapsed = ctimer_elapsed_seconds(timer);

  // Print best PV
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "  depth %d: value=%d, time=%.3fs, pv=", depth, value, elapsed);
  format_pvline_bench(sb, pv_line, game);
  append_outcome_bench(sb, game, value);
  printf("%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);

  // Print ranked root moves
  for (int i = 0; i < num_ranked_pvs; i++) {
    sb = string_builder_create();
    string_builder_add_formatted_string(sb, "    %2d. value=%d, pv=", i + 1,
                                        ranked_pvs[i].score);
    format_pvline_bench(sb, &ranked_pvs[i], game);
    append_outcome_bench(sb, game, ranked_pvs[i].score);
    printf("%s\n", string_builder_peek(sb));
    string_builder_destroy(sb);
  }
  (void)fflush(stdout);
}

void test_peg_pos6_endgame(void) {
  log_set_level(LOG_WARN);

  const char *cgp =
      "7B4T1E/5V1O3BO1n/2FEVERS3OF1A/"
      "WREN1N1H3NU1M/4ZOA4D2O/"
      "3JAM5I2U/4X2QUAINTER/"
      "3CEILI3G2S/PALEST5S3/"
      "I2L3GAWK4/TAILPIpE7/"
      "C2I2ADORNER2/H14/E14/"
      "D14 EEGNORY/ADIOTUY 370/452 0";

  Config *config = config_create_or_die(
      "set -lex NWL20 -threads 8 -s1 score -s2 score -r1 small -r2 small");
  exec_config_quiet_peg(config, "new");
  Game *game = config_get_game(config);

  ErrorStack *err = error_stack_create();
  game_load_cgp(game, cgp, err);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  int mover_idx = game_get_player_on_turn_index(game);
  int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  // Drain the bag.
  game_set_endgame_solving_mode(game);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  {
    Bag *bag = game_get_bag(game);
    for (int ml = 0; ml < ld_size; ml++)
      while (bag_get_letter(bag, ml) > 0)
        bag_draw_letter(bag, (MachineLetter)ml, mover_idx);
  }

  // Play 11K EYEN for mover.
  // Generate moves and find EYEN.
  MoveList *ml = move_list_create(512);
  {
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .thread_index = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  Move eyen_move;
  bool found_eyen = false;
  for (int i = 0; i < ml->count; i++) {
    const Move *mov = move_list_get_move(ml, i);
    char *ms = format_move(game, mov);
    if (strstr(ms, "EYEN") && strstr(ms, "11K")) {
      move_copy(&eyen_move, mov);
      found_eyen = true;
      printf("Playing: %s\n", ms);
      free(ms);
      break;
    }
    free(ms);
  }
  assert(found_eyen);
  play_move(&eyen_move, game, NULL);

  // Fix false game-end from playing on empty bag.
  if (game_get_game_end_reason(game) == GAME_END_REASON_STANDARD) {
    Equity bonus = calculate_end_rack_points(
        player_get_rack(game_get_player(game, opp_idx)), ld);
    player_add_to_score(game_get_player(game, mover_idx), -bonus);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }

  // Set racks for draw D scenario.
  // Unseen from mover's perspective: ADIOTUY + T = {A,D,I,O,T,T,U,Y}
  // Draw D: mover gets D added to remaining rack (GOR -> DGOR)
  // Opponent gets unseen - D = {A,I,O,T,T,U,Y}
  uint8_t unseen[MAX_ALPHABET_SIZE];
  bench_compute_unseen(game, mover_idx, unseen);

  // We need to figure out the ML for 'D'.
  MachineLetter ml_D = ld_hl_to_ml(ld, "D");

  Rack *opp_rack = player_get_rack(game_get_player(game, opp_idx));
  rack_reset(opp_rack);
  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t] - (t == ml_D ? 1 : 0);
    for (int k = 0; k < cnt; k++)
      rack_add_letter(opp_rack, (MachineLetter)t);
  }
  Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  rack_add_letter(mover_rack, ml_D);

  // Now opponent is on turn. Generate moves and find OUTPITCHED.
  {
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .thread_index = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  Move outpitched_move;
  bool found_otp = false;
  for (int i = 0; i < ml->count; i++) {
    const Move *mov = move_list_get_move(ml, i);
    char *ms = format_move(game, mov);
    if (strstr(ms, "OUT") && strstr(ms, "PITCHED")) {
      move_copy(&outpitched_move, mov);
      found_otp = true;
      printf("Playing: %s\n", ms);
      free(ms);
      break;
    }
    free(ms);
  }
  assert(found_otp);
  play_move(&outpitched_move, game, NULL);

  // Fix false game-end again.
  if (game_get_game_end_reason(game) == GAME_END_REASON_STANDARD) {
    Equity bonus = calculate_end_rack_points(
        player_get_rack(game_get_player(game, mover_idx)), ld);
    player_add_to_score(game_get_player(game, opp_idx), -bonus);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }

  // Show the position.
  printf("\n--- After EYEN + draw D + OUTPITCHED ---\n");
  {
    StringBuilder *sb = string_builder_create();
    string_builder_add_game(game, NULL, NULL, NULL, sb);
    char *s = string_builder_dump(sb, NULL);
    printf("%s", s);
    free(s);
    string_builder_destroy(sb);
  }

  // Generate top 20 moves for mover.
  {
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .thread_index = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  move_list_sort_moves(ml);
  int show = ml->count < 20 ? ml->count : 20;
  printf("\nTop %d of %d moves by equity:\n", show, ml->count);
  for (int i = 0; i < show; i++) {
    const Move *mov = move_list_get_move(ml, i);
    char *ms = format_move(game, mov);
    StringBuilder *eq_sb = string_builder_create();
    string_builder_add_equity(eq_sb, move_get_equity(mov), "%+.2f");
    char *eq_str = string_builder_dump(eq_sb, NULL);
    printf("  %2d. %-24s  equity=%s  score=%d\n", i + 1, ms, eq_str,
           equity_to_int(move_get_score(mov)));
    free(eq_str);
    string_builder_destroy(eq_sb);
    free(ms);
  }

  // Endgame solve with top 20 moves and per-ply ranked output.
  printf("\n--- Endgame Solve (top 20 variations) ---\n");
  (void)fflush(stdout);

  Timer eg_timer;
  ctimer_start(&eg_timer);

  TranspositionTable *shared_tt = transposition_table_create(0.5);
  EndgameSolver *solver = endgame_solver_create();
  EndgameResults *results = endgame_results_create();

  EndgameArgs ea = {
      .thread_control = config_get_thread_control(config),
      .game = game,
      .plies = MAX_SEARCH_DEPTH,
      .shared_tt = shared_tt,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 8,
      .use_heuristics = true,
      .num_top_moves = 20,
      .soft_time_limit = 10.0,
      .hard_time_limit = 30.0,
      .skip_word_pruning = true,
      .per_ply_callback = pos6_per_ply_callback,
      .per_ply_callback_data = &eg_timer,
  };
  ErrorStack *es = error_stack_create();
  endgame_solve(solver, &ea, results, es);
  assert(error_stack_is_empty(es));
  error_stack_destroy(es);

  char *pv_str =
      endgame_results_get_string(results, game, NULL, true);
  printf("%s\n", pv_str);
  free(pv_str);

  endgame_results_destroy(results);
  endgame_solver_destroy(solver);

  // --- Now play (O)D and show opponent's top moves ---
  printf("\n=== After mover plays 6A (O)D ===\n");

  // Find and play (O)D
  {
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .thread_index = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  Move od_move;
  bool found_od = false;
  for (int i = 0; i < ml->count; i++) {
    const Move *mov = move_list_get_move(ml, i);
    char *ms = format_move(game, mov);
    if (strstr(ms, "6A") && strstr(ms, "D") && move_get_score(mov) == int_to_equity(7)) {
      move_copy(&od_move, mov);
      found_od = true;
      printf("Playing: %s\n", ms);
      free(ms);
      break;
    }
    free(ms);
  }
  assert(found_od);
  play_move(&od_move, game, NULL);

  // Show position after (O)D
  {
    StringBuilder *sb = string_builder_create();
    string_builder_add_game(game, NULL, NULL, NULL, sb);
    char *s = string_builder_dump(sb, NULL);
    printf("%s", s);
    free(s);
    string_builder_destroy(sb);
  }

  // Generate opponent's top 20 moves
  {
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .thread_index = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  move_list_sort_moves(ml);
  show = ml->count < 20 ? ml->count : 20;
  printf("\nOpponent's top %d of %d moves by equity:\n", show, ml->count);
  for (int i = 0; i < show; i++) {
    const Move *mov = move_list_get_move(ml, i);
    char *ms = format_move(game, mov);
    StringBuilder *eq_sb = string_builder_create();
    string_builder_add_equity(eq_sb, move_get_equity(mov), "%+.2f");
    char *eq_str = string_builder_dump(eq_sb, NULL);
    printf("  %2d. %-24s  equity=%s  score=%d\n", i + 1, ms, eq_str,
           equity_to_int(move_get_score(mov)));
    free(eq_str);
    string_builder_destroy(eq_sb);
    free(ms);
  }

  // Endgame solve from opponent's perspective with top 20
  printf("\n--- Opponent Endgame Solve (top 20 variations) ---\n");
  (void)fflush(stdout);

  Timer eg_timer2;
  ctimer_start(&eg_timer2);

  EndgameSolver *solver2 = endgame_solver_create();
  EndgameResults *results2 = endgame_results_create();

  EndgameArgs ea2 = {
      .thread_control = config_get_thread_control(config),
      .game = game,
      .plies = MAX_SEARCH_DEPTH,
      .shared_tt = shared_tt,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 8,
      .use_heuristics = true,
      .num_top_moves = 20,
      .soft_time_limit = 10.0,
      .hard_time_limit = 30.0,
      .skip_word_pruning = true,
      .per_ply_callback = pos6_per_ply_callback,
      .per_ply_callback_data = &eg_timer2,
  };
  ErrorStack *es2 = error_stack_create();
  endgame_solve(solver2, &ea2, results2, es2);
  assert(error_stack_is_empty(es2));
  error_stack_destroy(es2);

  char *pv_str2 =
      endgame_results_get_string(results2, game, NULL, true);
  printf("%s\n", pv_str2);
  free(pv_str2);

  endgame_results_destroy(results2);
  endgame_solver_destroy(solver2);
  transposition_table_destroy(shared_tt);
  move_list_destroy(ml);
  config_destroy(config);
}

void test_peg_pos8(void) {
  log_set_level(LOG_FATAL);

  // Write position 8 CGP to a temp file.
  FILE *fp = fopen("/tmp/peg1_pos8.txt", "we");
  assert(fp);
  fprintf(fp,
          "3RELOANED4/2J4R1HELIO1/2EW2QI7/2TA3O2C4/"
          "3GAG1S2E4/4MONO2N4/4ODE3T4/4U1WUZ1r4/"
          "4R1S1I1E4/VIFFS1B1N1M4/2R3E1KOA4/"
          "HOYA1TAXYING3/2P1LITU7/1DARICS8/"
          "VANE11 ABELTU?/DEEIIPR 296/379 0\n");
  fclose(fp);

  PegBenchConfig peg = {
      .label = "4-stg {200,20,16}",
      .num_stages = 4,
      .stage_limits = {200, 20, 16},
      .num_limits = 3,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
      .first_win_spread_all_final = true,
      .tt_fraction = 0.5,
      .early_cutoff = true,
  };

  double soft_time = 2.0;
  double hard_time = 2.0;

  run_peg1_ab_benchmark("/tmp/peg1_pos8.txt", &peg, soft_time, hard_time, 1);
}
