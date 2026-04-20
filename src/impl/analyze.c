#include "analyze.h"

#include "../def/bai_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"
#include "../ent/win_pct.h"
#include "../str/endgame_string.h"
#include "../str/game_string.h"
#include "../str/inference_string.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../str/sim_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "move_gen.h"
#include "simmer.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  ANALYZE_NOTE_TRUNCATE_LEN = 30,
  ANALYZE_SUMMARY_COLS = 9,
  ANALYZE_SUMMARY_COL_PADDING = 2,
};

typedef enum {
  ANALYSIS_TYPE_STATIC,
  ANALYSIS_TYPE_SIM,
  ANALYSIS_TYPE_ENDGAME,
} analysis_type_t;

// Per-move data for best or actual play, used to print them side by side.
// rank_idx is the 0-based rank in the final sorted move list; -1 when
// unavailable.
typedef struct TurnResultMove {
  Move move;
  double equity;
  double win_pct;
  int endgame_spread;
  int endgame_value;
  int rank_idx;
} TurnResultMove;

// TurnResult stores typed values (Rack, Move) rather than pre-formatted
// strings so that formatting decisions are deferred to output time. note_str
// is a non-owning pointer into the GameHistory and requires no allocation.
// best and actual are always valid; when actual was the best play, best is a
// copy of actual (rank_idx == 0 on both). equity_lost and win_pct_lost are
// computed at display time from best/actual fields via compute_equity_lost /
// compute_win_pct_lost.
typedef struct TurnResult {
  int turn_number;
  int player_index;
  int event_idx;
  Rack rack;
  TurnResultMove best;
  TurnResultMove actual;
  const char *note_str; // owned by GameHistory; no alloc needed
  bool was_endgame;
  bool used_inference;
} TurnResult;

// Result of analyze_gen_moves_with_actual: the effective actual move and its
// pre-sort heap index (or -1 if it was inserted rather than generated).
typedef struct GenMovesResult {
  int heap_idx;
  Move actual_move;
} GenMovesResult;

static Equity compute_equity_lost(const TurnResult *tr) {
  return double_to_equity(tr->best.equity - tr->actual.equity);
}

static double compute_win_pct_lost(const TurnResult *tr) {
  if (tr->was_endgame) {
    const int best_spread = tr->best.endgame_spread;
    const int actual_spread = tr->actual.endgame_spread;
    if (best_spread == actual_spread) {
      return 0.0;
    }
    if (best_spread > 0 && actual_spread < 0) {
      return 100.0;
    }
    if ((best_spread >= 0 && actual_spread < 0) ||
        (best_spread > 0 && actual_spread == 0)) {
      return 50.0;
    }
    return 0.0;
  }
  if (tr->actual.win_pct < 0.0) {
    return 0.0; // static analysis: no meaningful win% difference
  }
  return tr->best.win_pct - tr->actual.win_pct;
}

struct AnalyzeCtx {
  Game *game;
  Game *actual_play_endgame;
  MoveList *move_list;
  SimResults *sim_results;
  EndgameResults *endgame_results;
  EndgameResults *actual_play_endgame_results;
  EndgameCtx *endgame_ctx;
  InferenceResults *inference_results;
  SimCtx *sim_ctx; // NULL initially; persisted across sim calls
  TurnResult *turn_results;
  int turn_results_count;
  int turn_results_capacity;
};

AnalyzeCtx *analyze_ctx_create(void) {
  return calloc_or_die(1, sizeof(AnalyzeCtx));
}

void analyze_ctx_destroy(AnalyzeCtx *ctx) {
  if (!ctx) {
    return;
  }
  game_destroy(ctx->game);
  game_destroy(ctx->actual_play_endgame);
  move_list_destroy(ctx->move_list);
  sim_results_destroy(ctx->sim_results);
  endgame_results_destroy(ctx->endgame_results);
  endgame_results_destroy(ctx->actual_play_endgame_results);
  endgame_ctx_destroy(ctx->endgame_ctx);
  inference_results_destroy(ctx->inference_results);
  sim_ctx_destroy(ctx->sim_ctx);
  free(ctx->turn_results);
  free(ctx);
}

// Initializes or refreshes the context from the current AnalyzeArgs. On the
// first call (ctx->game == NULL), all resources are created by duplicating
// the game held in sim_args. On subsequent calls, the game is updated from
// sim_args in-place and the move list capacity is resized.
static void analyze_ctx_init(AnalyzeCtx *ctx, const AnalyzeArgs *args) {
  const int num_plays = args->sim_args.num_plays;
  if (!ctx->game) {
    ctx->game = game_duplicate(args->sim_args.game);
    ctx->move_list = move_list_create(num_plays);
    ctx->sim_results = sim_results_create(0.0);
    ctx->endgame_results = endgame_results_create();
    ctx->endgame_ctx = NULL;
    ctx->inference_results = inference_results_create(NULL);
    ctx->sim_ctx = NULL;
  } else {
    game_copy(ctx->game, args->sim_args.game);
    move_list_resize(ctx->move_list, num_plays);
  }
  ctx->turn_results_count = 0;
}

// Appends a turn result to the context's array, expanding capacity as needed.
static void analyze_ctx_push_turn_result(AnalyzeCtx *ctx,
                                         const TurnResult *turn_result) {
  if (ctx->turn_results_count == ctx->turn_results_capacity) {
    ctx->turn_results_capacity =
        ctx->turn_results_capacity ? ctx->turn_results_capacity * 2 : 64;
    ctx->turn_results =
        realloc_or_die(ctx->turn_results,
                       sizeof(TurnResult) * (size_t)ctx->turn_results_capacity);
  }
  ctx->turn_results[ctx->turn_results_count++] = *turn_result;
}

static Game *analyze_ctx_get_actual_play_endgame(AnalyzeCtx *ctx,
                                                 const Game *source_game) {
  if (!ctx->actual_play_endgame) {
    ctx->actual_play_endgame = game_duplicate(source_game);
  } else {
    game_copy(ctx->actual_play_endgame, source_game);
  }
  return ctx->actual_play_endgame;
}

static EndgameResults *
analyze_ctx_get_actual_play_endgame_results(AnalyzeCtx *ctx) {
  if (!ctx->actual_play_endgame_results) {
    ctx->actual_play_endgame_results = endgame_results_create();
  }
  return ctx->actual_play_endgame_results;
}

// Builds a heap-allocated string for rack; caller must free.
static char *analyze_format_rack(const Rack *rack,
                                 const LetterDistribution *ld) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, rack, ld, false);
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}

// Builds a heap-allocated string for move; caller must free.
// If board is non-NULL, uses string_builder_add_move (shows played-through
// tiles from the board). If board is NULL, falls back to
// string_builder_add_move_description.
static char *analyze_format_move(const Move *move, const Board *board,
                                 const LetterDistribution *ld) {
  StringBuilder *sb = string_builder_create();
  if (board != NULL) {
    string_builder_add_move(sb, board, move, ld, false);
  } else {
    string_builder_add_move_description(sb, move, ld);
  }
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}

// Fills one row of the move comparison grid. All char* parameters transfer
// ownership to the grid. spread_str may be NULL for non-endgame grids.
static void add_move_comparison_row(StringGrid *move_sg, int row,
                                    const char *label, char *rank_str,
                                    char *move_str, char *leave_str,
                                    char *score_str, char *spread_str,
                                    char *wp_str, char *eq_str, char *steq_str,
                                    int wp_col, bool is_endgame) {
  int curr_col = 0;
  string_grid_set_cell(move_sg, row, curr_col++, string_duplicate(label));
  string_grid_set_cell(move_sg, row, curr_col++, rank_str);
  string_grid_set_cell(move_sg, row, curr_col++, move_str);
  string_grid_set_cell(move_sg, row, curr_col++, leave_str);
  string_grid_set_cell(move_sg, row, curr_col++, score_str);
  if (is_endgame) {
    string_grid_set_cell(move_sg, row, curr_col, spread_str);
  } else {
    free(spread_str);
  }
  curr_col = wp_col;
  string_grid_set_cell(move_sg, row, curr_col++, wp_str);
  string_grid_set_cell(move_sg, row, curr_col++, eq_str);
  string_grid_set_cell(move_sg, row, curr_col, steq_str);
}

// Fills rows 1 (Best), 2 (Actual), and 3 (Diff) of a 4-row move comparison
// grid. Row 0 (header) must be filled by the caller.
static void add_move_comparison_rows(StringGrid *move_sg, const TurnResult *tr,
                                     const Board *board,
                                     const LetterDistribution *ld,
                                     bool is_endgame) {
  const TurnResultMove *best_trm = &tr->best;
  const TurnResultMove *actual_trm = &tr->actual;
  const Rack *rack = &tr->rack;
  const int wp_col = is_endgame ? 6 : 5;

  const double best_st_eq =
      move_get_type(&best_trm->move) == GAME_EVENT_PASS
          ? EQUITY_PASS_DISPLAY_DOUBLE
          : equity_to_double(move_get_equity(&best_trm->move));
  const double actual_st_eq =
      move_get_type(&actual_trm->move) == GAME_EVENT_PASS
          ? EQUITY_PASS_DISPLAY_DOUBLE
          : equity_to_double(move_get_equity(&actual_trm->move));

  const Equity equity_lost = compute_equity_lost(tr);
  const double win_pct_lost = compute_win_pct_lost(tr);

  StringBuilder *sb = string_builder_create();

  // Best row (row 1): best is always rank 1.
  string_builder_add_move(sb, board, &best_trm->move, ld, false);
  char *best_move_str = string_builder_dump(sb, NULL);
  string_builder_clear(sb);
  string_builder_add_move_leave(sb, rack, &best_trm->move, ld);
  char *best_leave_str = string_builder_dump(sb, NULL);
  string_builder_clear(sb);
  add_move_comparison_row(
      move_sg, 1, "Best", string_duplicate("1"), best_move_str, best_leave_str,
      get_formatted_string("%d",
                           equity_to_int(move_get_score(&best_trm->move))),
      is_endgame ? get_formatted_string("%d", best_trm->endgame_spread) : NULL,
      best_trm->win_pct >= 0.0 ? get_formatted_string("%.2f", best_trm->win_pct)
                               : string_duplicate("-"),
      best_trm->win_pct >= 0.0 ? get_formatted_string("%.2f", best_trm->equity)
                               : string_duplicate("-"),
      is_endgame ? string_duplicate("-")
                 : get_formatted_string("%.2f", best_st_eq),
      wp_col, is_endgame);

  // Actual row (row 2).
  string_builder_add_move(sb, board, &actual_trm->move, ld, false);
  char *actual_move_str = string_builder_dump(sb, NULL);
  string_builder_clear(sb);
  string_builder_add_move_leave(sb, rack, &actual_trm->move, ld);
  char *actual_leave_str = string_builder_dump(sb, NULL);
  string_builder_clear(sb);
  add_move_comparison_row(
      move_sg, 2, "Actual",
      actual_trm->rank_idx >= 0
          ? get_formatted_string("%d", actual_trm->rank_idx + 1)
          : string_duplicate("-"),
      actual_move_str, actual_leave_str,
      get_formatted_string("%d",
                           equity_to_int(move_get_score(&actual_trm->move))),
      is_endgame ? get_formatted_string("%d", actual_trm->endgame_spread)
                 : NULL,
      actual_trm->win_pct >= 0.0
          ? get_formatted_string("%.2f", actual_trm->win_pct)
          : string_duplicate("-"),
      actual_trm->win_pct >= 0.0
          ? get_formatted_string("%.2f", actual_trm->equity)
          : string_duplicate("-"),
      is_endgame ? string_duplicate("-")
                 : get_formatted_string("%.2f", actual_st_eq),
      wp_col, is_endgame);

  // Diff row (row 3): dashes for R/Mv/Lv; score diff for S[/Spr]; diffs for
  // Wp/Eq/StEq.
  const int score_diff = equity_to_int(move_get_score(&best_trm->move)) -
                         equity_to_int(move_get_score(&actual_trm->move));
  add_move_comparison_row(
      move_sg, 3, "Diff", string_duplicate("-"), string_duplicate("-"),
      string_duplicate("-"), get_formatted_string("%d", score_diff),
      is_endgame ? get_formatted_string("%d", best_trm->endgame_spread -
                                                  actual_trm->endgame_spread)
                 : NULL,
      get_formatted_string("%.2f", win_pct_lost),
      get_formatted_string("%.2f", equity_to_double(equity_lost)),
      is_endgame ? string_duplicate("-")
                 : get_formatted_string("%.2f", best_st_eq - actual_st_eq),
      wp_col, is_endgame);

  string_builder_destroy(sb);
}

static void write_per_turn_human_readable(
    const TurnResult *turn_result, const GameHistory *game_history,
    const LetterDistribution *ld, const char *report_path,
    const AnalyzeCtx *ctx, int max_num_display_plays, ErrorStack *error_stack) {
  const char *player_name =
      game_history_player_get_nickname(game_history, turn_result->player_index);

  const Board *board = game_get_board(ctx->game);
  char *rack_str = analyze_format_rack(&turn_result->rack, ld);
  char *actual_str = analyze_format_move(&turn_result->actual.move, board, ld);

  StringBuilder *sb = string_builder_create();

  // Header: "--- Event N: <play> <rack> (<player>) ---"
  string_builder_add_formatted_string(sb, "--- Event %d: %s %s (%s) ---\n\n",
                                      turn_result->turn_number, actual_str,
                                      rack_str, player_name);
  free(rack_str);
  free(actual_str);

  const bool is_endgame = turn_result->was_endgame;
  const bool is_sim = !is_endgame && turn_result->actual.win_pct >= 0.0;

  char *sim_str = NULL;
  if (is_sim) {
    sim_str = sim_results_get_string(ctx->game, ctx->sim_results,
                                     max_num_display_plays, 2, -1, -1, NULL, 0,
                                     false, false, NULL);
  }

  // Per-turn move grid: label, R, Mv, Lv, S, [Spr (endgame only),] Wp, Eq,
  // StEq. 4 rows: header + best + actual + diff.
  // For endgames: 9 cols with 'Spr' at col 5 (final spread); StEq shows '-'.
  const int num_grid_cols = is_endgame ? 9 : 8;
  StringGrid *move_sg = string_grid_create(4, num_grid_cols, 2);

  // Header row.
  int curr_row = 0;
  int curr_col = 0;
  string_grid_set_cell(move_sg, curr_row, curr_col++, string_duplicate(""));
  string_grid_set_cell(move_sg, curr_row, curr_col++, string_duplicate("R"));
  string_grid_set_cell(move_sg, curr_row, curr_col++, string_duplicate("Mv"));
  string_grid_set_cell(move_sg, curr_row, curr_col++, string_duplicate("Lv"));
  string_grid_set_cell(move_sg, curr_row, curr_col++, string_duplicate("S"));
  if (is_endgame) {
    string_grid_set_cell(move_sg, curr_row, curr_col++,
                         string_duplicate("Spr"));
  }
  string_grid_set_cell(move_sg, curr_row, curr_col++, string_duplicate("Wp"));
  string_grid_set_cell(move_sg, curr_row, curr_col++, string_duplicate("Eq"));
  string_grid_set_cell(move_sg, curr_row, curr_col, string_duplicate("StEq"));

  add_move_comparison_rows(move_sg, turn_result, board, ld, is_endgame);

  string_builder_add_string_grid(sb, move_sg, false);
  string_builder_add_string(sb, "\n");
  string_grid_destroy(move_sg);

  // Board state before the actual play (plain text, no colors).
  string_builder_add_game(ctx->game, NULL, NULL, game_history, sb);

  // Sim results, endgame results, or static move list.
  if (is_sim) {
    string_builder_add_formatted_string(sb, "%s\n", sim_str);
    free(sim_str);
  } else if (is_endgame) {
    const size_t len_before = string_builder_length(sb);
    string_builder_endgame_single_pv(sb, ctx->endgame_results, ctx->game,
                                     game_history, 0);
    if (string_builder_length(sb) > len_before) {
      string_builder_add_string(sb, "\n");
    }
  } else {
    char *ml_str = move_list_get_string(
        ctx->move_list, game_get_board(ctx->game), ld, max_num_display_plays,
        -1, -1, NULL, 0, false, false, NULL);
    if (ml_str) {
      string_builder_add_formatted_string(sb, "%s\n", ml_str);
      free(ml_str);
    }
  }

  // Inference result, if available.
  if (turn_result->used_inference) {
    char *infer_str =
        inference_result_get_string(ctx->inference_results, ld, 20, false);
    if (infer_str) {
      string_builder_add_formatted_string(sb, "%s\n", infer_str);
      free(infer_str);
    }
  }

  char *section = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  append_string_to_file(report_path, section, error_stack);
  free(section);
}

static void write_per_turn_csv(const TurnResult *turn_result,
                               const GameHistory *game_history,
                               const LetterDistribution *ld,
                               const char *report_path, bool is_first_turn,
                               ErrorStack *error_stack) {
  StringBuilder *sb = string_builder_create();

  if (is_first_turn) {
    string_builder_add_string(
        sb, "turn,player,rack,actual,best,equity_lost,win_pct_lost\n");
  }

  const char *player_name =
      game_history_player_get_nickname(game_history, turn_result->player_index);

  char *rack_str = analyze_format_rack(&turn_result->rack, ld);
  char *actual_str = analyze_format_move(&turn_result->actual.move, NULL, ld);
  char *best_str = turn_result->actual.rank_idx != 0
                       ? analyze_format_move(&turn_result->best.move, NULL, ld)
                       : string_duplicate("-");

  string_builder_add_formatted_string(
      sb, "%d,%s,%s,%s,%s,%.2f,%.2f\n", turn_result->turn_number, player_name,
      rack_str, actual_str, best_str,
      equity_to_double(compute_equity_lost(turn_result)),
      compute_win_pct_lost(turn_result));

  free(rack_str);
  free(actual_str);
  free(best_str);

  char *section = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  append_string_to_file(report_path, section, error_stack);
  free(section);
}

// Fills one data row of the analysis summary grid for turn_result and updates
// running totals. Replays game state to event_idx so that board-resolved move
// strings are accurate.
static void add_summary_event_row(StringGrid *sg, int row, const TurnResult *tr,
                                  GameHistory *game_history, Game *game,
                                  const LetterDistribution *ld,
                                  int player_turn_number,
                                  Equity *total_equity_lost,
                                  double *total_win_pct_lost,
                                  ErrorStack *error_stack) {
  game_play_n_events(game_history, game, tr->event_idx, false, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    log_fatal("unexpected error replaying game history events for summary "
              "table: %s",
              error_stack_top(error_stack));
  }
  game_history_goto(game_history, tr->event_idx, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    log_fatal("unexpected error updating game history current event for "
              "summary table: %s",
              error_stack_top(error_stack));
  }

  const Board *board = game_get_board(game);
  const char *player_name =
      game_history_player_get_nickname(game_history, tr->player_index);

  int curr_col = 0;
  string_grid_set_cell(sg, row, curr_col++,
                       get_formatted_string("%d", tr->turn_number));
  string_grid_set_cell(sg, row, curr_col++,
                       get_formatted_string("%d", player_turn_number));
  string_grid_set_cell(sg, row, curr_col++, string_duplicate(player_name));
  string_grid_set_cell(sg, row, curr_col++, analyze_format_rack(&tr->rack, ld));
  string_grid_set_cell(sg, row, curr_col++,
                       analyze_format_move(&tr->actual.move, board, ld));
  string_grid_set_cell(sg, row, curr_col++,
                       tr->actual.rank_idx != 0
                           ? analyze_format_move(&tr->best.move, board, ld)
                           : string_duplicate("-"));
  string_grid_set_cell(sg, row, curr_col++,
                       get_formatted_string("%.2f", compute_win_pct_lost(tr)));
  string_grid_set_cell(
      sg, row, curr_col++,
      get_formatted_string("%.2f", equity_to_double(compute_equity_lost(tr))));

  if (tr->note_str) {
    char *trimmed_note = string_duplicate(tr->note_str);
    trim_whitespace(trimmed_note);
    const size_t note_len = string_length(trimmed_note);
    if ((int)note_len > ANALYZE_NOTE_TRUNCATE_LEN) {
      char *truncated =
          get_substring(trimmed_note, 0, ANALYZE_NOTE_TRUNCATE_LEN);
      char *with_ellipsis = get_formatted_string("%s...", truncated);
      free(truncated);
      free(trimmed_note);
      string_grid_set_cell(sg, row, curr_col, with_ellipsis);
    } else {
      string_grid_set_cell(sg, row, curr_col, trimmed_note);
    }
  } else {
    string_grid_set_cell(sg, row, curr_col, string_duplicate(""));
  }

  *total_equity_lost += compute_equity_lost(tr);
  *total_win_pct_lost += compute_win_pct_lost(tr);
}

// Writes a game summary table for turns matching player_filter.
// player_filter == -1 means include all players (combined summary).
// matching_count is pre-computed by the caller from per-game turn tracking.
static void write_analysis_summary(GameHistory *game_history,
                                   const LetterDistribution *ld,
                                   const TurnResult *turn_results,
                                   int num_turn_results,
                                   const char *report_path, int player_filter,
                                   int matching_count, Game *game,
                                   ErrorStack *error_stack) {
  if (matching_count == 0) {
    return;
  }

  StringBuilder *header_sb = string_builder_create();
  if (player_filter == -1) {
    string_builder_add_string(header_sb, "\n=== Combined Game Summary ===\n\n");
  } else {
    const char *player_name =
        game_history_player_get_nickname(game_history, player_filter);
    string_builder_add_formatted_string(
        header_sb, "\n=== Game Summary: %s ===\n\n", player_name);
  }
  char *header_str = string_builder_dump(header_sb, NULL);
  string_builder_destroy(header_sb);
  append_string_to_file(report_path, header_str, error_stack);
  free(header_str);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const int num_rows = matching_count + 3; // header + data rows + Total + Avg
  StringGrid *sg = string_grid_create(num_rows, ANALYZE_SUMMARY_COLS,
                                      ANALYZE_SUMMARY_COL_PADDING);

  int curr_row = 0;
  int curr_col = 0;
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("T"));
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("PT"));
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("P"));
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Rack"));
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Actual"));
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Best"));
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("WPL"));
  string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("EqL"));
  string_grid_set_cell(sg, curr_row, curr_col, string_duplicate("Note"));

  Equity total_equity_lost = 0;
  double total_win_pct_lost = 0.0;
  int grid_row = 1;
  int player_turn_counts[2] = {0, 0};

  for (int result_idx = 0; result_idx < num_turn_results; result_idx++) {
    const TurnResult *tr = &turn_results[result_idx];
    player_turn_counts[tr->player_index]++;
    if (player_filter != -1 && tr->player_index != player_filter) {
      continue;
    }
    add_summary_event_row(sg, grid_row, tr, game_history, game, ld,
                          player_turn_counts[tr->player_index],
                          &total_equity_lost, &total_win_pct_lost, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }
    grid_row++;
  }

  if (error_stack_is_empty(error_stack)) {
    const double avg_equity =
        equity_to_double(total_equity_lost) / (double)matching_count;
    const double avg_win_pct = total_win_pct_lost / (double)matching_count;

    // Total row: label in Best column, values in WPL and EqL columns.
    curr_row = matching_count + 1;
    curr_col = 5;
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Total"));
    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%.2f", total_win_pct_lost));
    string_grid_set_cell(
        sg, curr_row, curr_col,
        get_formatted_string("%.2f", equity_to_double(total_equity_lost)));
    // Avg row: label in Best column, values in WPL and EqL columns.
    curr_row++;
    curr_col = 5;
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Avg"));
    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%.2f", avg_win_pct));
    string_grid_set_cell(sg, curr_row, curr_col,
                         get_formatted_string("%.2f", avg_equity));

    StringBuilder *sb = string_builder_create();
    string_builder_add_string_grid(sb, sg, false);
    string_builder_add_string(sb, "\n");
    char *section = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    append_string_to_file(report_path, section, error_stack);
    free(section);
  }

  string_grid_destroy(sg);
}

// Generates moves for the current game state, then ensures the actual move
// appears in the heap. If the move is a phony (opponent would challenge it
// off), the effective actual move is treated as a pass. On an opening move of
// a transposable board, replaces the movegen-generated horizontal equivalent
// with the actual vertical play while preserving the computed equity.
// Returns a GenMovesResult with the actual move pointer and its pre-sort heap
// index (heap_idx == -1 if the move was inserted rather than generated).
static GenMovesResult
analyze_gen_moves_with_actual(AnalyzeCtx *ctx, const ValidatedMoves *vms,
                              bool vertical_opening_is_transposable) {
  Move actual_move;
  if (validated_moves_is_phony(vms, 0)) {
    move_set_as_pass(&actual_move);
  } else {
    actual_move = *validated_moves_get_move(vms, 0);
  }

  move_list_reset(ctx->move_list);
  const MoveGenArgs gen_args = {
      .game = ctx->game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = ctx->move_list,
  };
  generate_moves(&gen_args);

  // Check whether the actual move is already in the generated heap.
  // On the opening move of a transposable board, also accept the horizontal
  // equivalent of a vertical play: replace it in-place so the player's actual
  // move (with its position and direction) is used for display while keeping
  // the movegen-computed equity intact for accurate ranking.
  const int move_count = move_list_get_count(ctx->move_list);
  int actual_move_heap_idx = -1;
  for (int move_idx = 0; move_idx < move_count; move_idx++) {
    if (compare_moves_without_equity(
            move_list_get_move(ctx->move_list, move_idx), &actual_move, true) ==
        -1) {
      actual_move_heap_idx = move_idx;
      break;
    }
    if (vertical_opening_is_transposable &&
        board_get_tiles_played(game_get_board(ctx->game)) == 0 &&
        moves_are_transposed(move_list_get_move(ctx->move_list, move_idx),
                             &actual_move)) {
      Move *gen_move = move_list_get_move(ctx->move_list, move_idx);
      const Equity gen_equity = move_get_equity(gen_move);
      move_copy(gen_move, &actual_move);
      move_set_equity(gen_move, gen_equity);
      actual_move_heap_idx = move_idx;
      break;
    }
  }
  if (actual_move_heap_idx >= 0) {
    return (GenMovesResult){.heap_idx = actual_move_heap_idx,
                            .actual_move = actual_move};
  }
  if (move_list_get_count(ctx->move_list) ==
      move_list_get_capacity(ctx->move_list)) {
    move_list_pop_move(ctx->move_list);
  }
  move_list_add_move(ctx->move_list, &actual_move);
  return (GenMovesResult){.heap_idx = -1, .actual_move = actual_move};
}

// Analyzes a single turn statically (sim_plies == 0): generates moves, finds
// the best, and records equity lost against the actual play.
static void analyze_with_static(const GameEvent *event, TurnResult *turn_result,
                                const GameHistory *game_history,
                                const LetterDistribution *ld,
                                const char *report_path, AnalyzeCtx *ctx,
                                int max_num_display_plays, bool human_readable,
                                bool is_first_analyzed_turn,
                                bool vertical_opening_is_transposable,
                                ErrorStack *error_stack) {
  const ValidatedMoves *vms = game_event_get_vms(event);
  const GenMovesResult gmr =
      analyze_gen_moves_with_actual(ctx, vms, vertical_opening_is_transposable);

  // Sort into descending equity order: index 0 is the best move.
  move_list_sort_moves(ctx->move_list);

  // Find the actual move in the sorted list to get its 0-based rank.
  const int sorted_count = move_list_get_count(ctx->move_list);
  int actual_move_rank_idx = -1;
  for (int rank_idx = 0; rank_idx < sorted_count; rank_idx++) {
    if (compare_moves_without_equity(
            move_list_get_move(ctx->move_list, rank_idx), &gmr.actual_move,
            true) == -1) {
      actual_move_rank_idx = rank_idx;
      break;
    }
  }
  if (actual_move_rank_idx < 0) {
    log_fatal(
        "failed to find actual move in heap after generation, insertion, and "
        "sorting");
  }

  turn_result->actual.move = gmr.actual_move;
  turn_result->actual.win_pct = -1.0;
  turn_result->actual.rank_idx = actual_move_rank_idx;
  turn_result->best.win_pct = -1.0;
  turn_result->best.rank_idx = 0;

  if (actual_move_rank_idx == 0 || sorted_count == 0) {
    // Actual is the best move; best is a copy of actual.
    turn_result->best.move = turn_result->actual.move;
    turn_result->best.equity =
        equity_to_double(move_get_equity(&turn_result->actual.move));
    turn_result->actual.equity = turn_result->best.equity;
  } else {
    turn_result->best.move = *move_list_get_move(ctx->move_list, 0);
    turn_result->best.equity =
        equity_to_double(move_get_equity(&turn_result->best.move));
    turn_result->actual.equity = equity_to_double(move_get_equity(
        move_list_get_move(ctx->move_list, actual_move_rank_idx)));
  }
  turn_result->was_endgame = false;
  turn_result->used_inference = false;

  if (human_readable) {
    write_per_turn_human_readable(turn_result, game_history, ld, report_path,
                                  ctx, max_num_display_plays, error_stack);
  } else {
    write_per_turn_csv(turn_result, game_history, ld, report_path,
                       is_first_analyzed_turn, error_stack);
  }
}

// Analyzes a single turn using simulation. The sim_args in analyze_args must
// already be partially filled (thread_control, win_pcts, num_plays, etc.);
// this function sets the per-turn fields (game, move_list, known_opp_rack,
// inference_results) before calling simulate.
static void analyze_with_sim(const GameEvent *event, TurnResult *turn_result,
                             const GameHistory *game_history,
                             const LetterDistribution *ld,
                             const char *report_path, AnalyzeCtx *ctx,
                             AnalyzeArgs *args, bool is_first_analyzed_turn,
                             bool vertical_opening_is_transposable,
                             ErrorStack *error_stack) {
  const ValidatedMoves *vms = game_event_get_vms(event);
  const GenMovesResult gmr =
      analyze_gen_moves_with_actual(ctx, vms, vertical_opening_is_transposable);
  int actual_move_heap_idx = gmr.heap_idx;

  // Resolve heap index for BAI arm_avoid_prune when the move was inserted.
  const int heap_count = move_list_get_count(ctx->move_list);
  if (actual_move_heap_idx < 0) {
    for (int move_idx = 0; move_idx < heap_count; move_idx++) {
      if (compare_moves_without_equity(
              move_list_get_move(ctx->move_list, move_idx), &gmr.actual_move,
              true) == -1) {
        actual_move_heap_idx = move_idx;
        break;
      }
    }
    if (actual_move_heap_idx < 0) {
      log_fatal(
          "failed to find actual move in heap after generation and insertion");
    }
  }

  const int num_moves = move_list_get_count(ctx->move_list);
  const int player_on_turn_index = game_get_player_on_turn_index(ctx->game);
  Rack *known_opp_rack =
      player_get_rack(game_get_player(ctx->game, 1 - player_on_turn_index));

  // Suppress inference when the opponent's rack is already fully known, or
  // when the opponent just played a full rack (their leave was empty so there
  // is nothing to infer from).
  const bool opp_rack_fully_known =
      rack_get_total_letters(known_opp_rack) == RACK_SIZE;

  const int opponent_index = 1 - player_on_turn_index;
  bool opp_played_full_rack = false;
  for (int search_idx = turn_result->event_idx - 1; search_idx >= 0;
       search_idx--) {
    const GameEvent *opp_event =
        game_history_get_event(game_history, search_idx);
    const game_event_t opp_event_type = game_event_get_type(opp_event);
    if (opp_event_type != GAME_EVENT_TILE_PLACEMENT_MOVE &&
        opp_event_type != GAME_EVENT_EXCHANGE &&
        opp_event_type != GAME_EVENT_PASS) {
      continue;
    }
    if (game_event_get_player_index(opp_event) != opponent_index) {
      continue;
    }
    if (opp_event_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const ValidatedMoves *opp_vms = game_event_get_vms(opp_event);
      const Move *opp_move = validated_moves_get_move(opp_vms, 0);
      opp_played_full_rack = (move_get_tiles_played(opp_move) == RACK_SIZE);
    }
    break;
  }

  // Set per-turn fields then simulate.
  args->sim_args.game = ctx->game;
  args->sim_args.move_list = ctx->move_list;
  args->sim_args.known_opp_rack = known_opp_rack;
  args->sim_args.inference_results = ctx->inference_results;
  const bool original_infer_arg = args->sim_args.use_inference;
  const bool use_inference_for_this_turn =
      original_infer_arg && turn_result->event_idx > 0 &&
      !opp_rack_fully_known && !opp_played_full_rack;
  args->sim_args.use_inference = use_inference_for_this_turn;

  // When inference is enabled, pre-set nontarget_known_rack to the current
  // player's actual GCG rack before calling simulate. Without this,
  // populate_inference_args_with_game_history would copy the nontarget
  // player's rack from the replayed game state, which is empty (their tiles
  // are in the bag at that point). An empty nontarget rack lets inference
  // enumerate leaves that include tiles the current player actually holds,
  // causing set_random_rack to fatally fail when simulation tries to draw
  // those tiles from the bag. populate_inference_args_with_game_history only
  // overwrites nontarget_known_rack when it is empty, so pre-setting it here
  // prevents that override while leaving all other callers unaffected.
  if (use_inference_for_this_turn) {
    rack_copy(args->sim_args.inference_args.nontarget_known_rack,
              &turn_result->rack);
  }

  // Designate the player's actual move as not prunable so BAI always fully
  // simulates it.
  args->sim_args.bai_options.arm_avoid_prune = &actual_move_heap_idx;
  args->sim_args.bai_options.num_arm_avoid_prune = 1;

  simulate(&args->sim_args, &ctx->sim_ctx, ctx->sim_results, error_stack);

  args->sim_args.bai_options.arm_avoid_prune = NULL;
  args->sim_args.bai_options.num_arm_avoid_prune = 0;
  args->sim_args.use_inference = original_infer_arg;

  // Reset inference racks so they do not carry over to the next turn.
  if (use_inference_for_this_turn) {
    rack_reset(args->sim_args.inference_args.nontarget_known_rack);
    rack_reset(args->sim_args.inference_args.target_known_rack);
  }
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  sim_results_lock_and_sort_display_simmed_plays(ctx->sim_results);
  sim_results_unlock_display_infos(ctx->sim_results);

  const int best_move_index = sim_results_get_best_move_index(ctx->sim_results);
  if (best_move_index < 0) {
    log_fatal("failed to find best move in simmed plays after simulation");
  }
  const SimmedPlay *best_sp =
      sim_results_get_simmed_play(ctx->sim_results, best_move_index);
  const SimmedPlay *actual_sp = NULL;

  int actual_display_idx = -1;
  for (int sp_idx = 0; sp_idx < num_moves; sp_idx++) {
    const SimmedPlay *sp =
        sim_results_get_simmed_play(ctx->sim_results, sp_idx);
    const Move *sp_move = simmed_play_get_move(sp);
    if (compare_moves_without_equity(sp_move, &gmr.actual_move, true) == -1) {
      actual_sp = sp;
      actual_display_idx = sp_idx;
      break;
    }
  }

  if (actual_sp == NULL) {
    log_fatal("failed to find actual move in simmed plays after simulation");
  }

  turn_result->actual.move = gmr.actual_move;
  turn_result->actual.win_pct =
      stat_get_mean(simmed_play_get_win_pct_stat(actual_sp)) * 100.0;
  turn_result->actual.equity =
      stat_get_mean(simmed_play_get_equity_stat(actual_sp));

  turn_result->best.rank_idx = 0;
  if (best_sp == actual_sp) {
    // Actual is the best move; best is a copy of actual.
    turn_result->best.move = turn_result->actual.move;
    turn_result->best.win_pct = turn_result->actual.win_pct;
    turn_result->best.equity = turn_result->actual.equity;
    turn_result->actual.rank_idx = 0;
  } else {
    turn_result->best.move = *simmed_play_get_move(best_sp);
    turn_result->best.win_pct =
        stat_get_mean(simmed_play_get_win_pct_stat(best_sp)) * 100.0;
    turn_result->best.equity =
        stat_get_mean(simmed_play_get_equity_stat(best_sp));
    turn_result->actual.rank_idx = actual_display_idx;
  }
  turn_result->was_endgame = false;
  turn_result->used_inference = use_inference_for_this_turn;

  if (args->human_readable) {
    write_per_turn_human_readable(turn_result, game_history, ld, report_path,
                                  ctx, args->max_num_display_plays,
                                  error_stack);
  } else {
    write_per_turn_csv(turn_result, game_history, ld, report_path,
                       is_first_analyzed_turn, error_stack);
  }
}

static void set_turn_result_endgame_move(TurnResultMove *turn_result_move,
                                         const Move *move, const int value,
                                         const int spread, const int rank_idx) {

  turn_result_move->move = *move;
  turn_result_move->endgame_value = value;
  turn_result_move->endgame_spread = spread;
  if (spread > 0) {
    turn_result_move->win_pct = 100.0;
  } else if (spread < 0) {
    turn_result_move->win_pct = 0.0;
  } else {
    turn_result_move->win_pct = 50.0;
  }
  turn_result_move->equity = (double)turn_result_move->endgame_value;
  turn_result_move->rank_idx = rank_idx;
}

// Analyzes a single turn using the endgame solver. The endgame_args in
// analyze_args must already be partially filled; this function sets the
// per-turn game field before calling endgame_solve.
static void analyze_with_endgame(const GameEvent *event,
                                 TurnResult *turn_result,
                                 const GameHistory *game_history,
                                 const LetterDistribution *ld,
                                 const char *report_path, AnalyzeCtx *ctx,
                                 AnalyzeArgs *args, bool is_first_analyzed_turn,
                                 ErrorStack *error_stack) {
  args->endgame_args.game = ctx->game;
  endgame_solve(&ctx->endgame_ctx, &args->endgame_args, ctx->endgame_results,
                error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const PVLine *best_pvline =
      endgame_results_get_pvline(ctx->endgame_results, ENDGAME_RESULT_BEST);

  if (best_pvline->num_moves == 0) {
    log_fatal("endgame solver failed to find any moves for the position");
  }

  const ValidatedMoves *vms = game_event_get_vms(event);
  Move actual_move;
  if (validated_moves_is_phony(vms, 0)) {
    move_set_as_pass(&actual_move);
  } else {
    actual_move = *validated_moves_get_move(vms, 0);
  }

  Move best_move;
  small_move_to_move(&best_move, &best_pvline->moves[0],
                     game_get_board(ctx->game));
  const bool actual_is_best =
      compare_moves_without_equity(&best_move, &actual_move, true) == -1;

  set_turn_result_endgame_move(
      &turn_result->best, &best_move,
      endgame_results_get_value(ctx->endgame_results, ENDGAME_RESULT_BEST),
      endgame_results_get_spread(ctx->endgame_results, ENDGAME_RESULT_BEST,
                                 ctx->game),
      0);

  if (actual_is_best) {
    // Actual move is optimal; best is a copy of actual.
    turn_result->actual = turn_result->best;
  } else {
    // Solve from the position after the actual move to evaluate it. The
    // endgame solver runs from the opponent's perspective after the actual
    // move is played, so its value must be sign-flipped to get the current
    // player's spread. We only compare spreads (not values), because the
    // two solves are one turn apart and their absolute values are not
    // directly comparable.
    Game *actual_game_copy =
        analyze_ctx_get_actual_play_endgame(ctx, ctx->game);
    play_move(&actual_move, actual_game_copy, NULL);

    EndgameResults *actual_play_endgame_results =
        analyze_ctx_get_actual_play_endgame_results(ctx);
    args->endgame_args.game = actual_game_copy;
    endgame_solve(&ctx->endgame_ctx, &args->endgame_args,
                  actual_play_endgame_results, error_stack);

    if (!error_stack_is_empty(error_stack)) {
      return;
    }

    const int actual_endgame_spread = -endgame_results_get_spread(
        actual_play_endgame_results, ENDGAME_RESULT_BEST, actual_game_copy);
    const int spread_diff =
        turn_result->best.endgame_spread - actual_endgame_spread;
    if (spread_diff == 0) {
      // Actual move achieves the same spread as the best; treat as optimal.
      turn_result->actual = turn_result->best;
      turn_result->actual.move = actual_move;
      turn_result->best.move = actual_move;
    } else {
      set_turn_result_endgame_move(&turn_result->actual, &actual_move,
                                   turn_result->best.endgame_value -
                                       spread_diff,
                                   actual_endgame_spread, -1);
    }
  }

  turn_result->was_endgame = true;
  turn_result->used_inference = false;

  if (args->human_readable) {
    write_per_turn_human_readable(turn_result, game_history, ld, report_path,
                                  ctx, 0, error_stack);
  } else {
    write_per_turn_csv(turn_result, game_history, ld, report_path,
                       is_first_analyzed_turn, error_stack);
  }
}

// Analyzes all move events in game_history. Creates *analyze_ctx if NULL
// on entry; the caller is responsible for calling analyze_ctx_destroy after
// this returns.
void analyze_game(AnalyzeArgs *analyze_args, AnalyzeCtx **analyze_ctx,
                  ErrorStack *error_stack) {
  if (!*analyze_ctx) {
    *analyze_ctx = analyze_ctx_create();
  }
  AnalyzeCtx *ctx = *analyze_ctx;
  analyze_ctx_init(ctx, analyze_args);

  GameHistory *game_history = analyze_args->game_history;
  const char *report_path = analyze_args->report_path;

  // Write report header, creating/truncating the file.
  StringBuilder *header_sb = string_builder_create();
  const char *gcg_filename_header = game_history_get_gcg_filename(game_history);
  if (gcg_filename_header) {
    string_builder_add_formatted_string(header_sb, "%s\n", gcg_filename_header);
  }
  if (analyze_args->config_settings_str) {
    string_builder_add_formatted_string(header_sb, "%s\n\n",
                                        analyze_args->config_settings_str);
  }
  char *header_str = string_builder_dump(header_sb, NULL);
  string_builder_destroy(header_sb);
  write_string_to_file(report_path, "w", header_str, error_stack);
  free(header_str);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const LetterDistribution *ld = game_get_ld(ctx->game);
  const int num_events = game_history_get_num_events(game_history);
  int turn_counter = 0;
  bool is_first_analyzed_turn = true;
  int player_turn_counts[2] = {0, 0};
  const bool vertical_opening_is_transposable =
      board_is_vertical_opening_transposable(game_get_board(ctx->game));

  for (int event_idx = 0; event_idx < num_events; event_idx++) {
    const GameEvent *event = game_history_get_event(game_history, event_idx);
    const game_event_t event_type = game_event_get_type(event);

    if (event_type != GAME_EVENT_TILE_PLACEMENT_MOVE &&
        event_type != GAME_EVENT_EXCHANGE && event_type != GAME_EVENT_PASS) {
      continue;
    }

    turn_counter++;
    const int player_index = game_event_get_player_index(event);

    if (!analyze_args->analyze_players[player_index]) {
      continue;
    }

    game_play_n_events(game_history, ctx->game, event_idx, false, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }
    // Set num_played_events so inference can find the most recent move event
    // via game_history_get_most_recent_move_event_index.
    game_history_goto(game_history, event_idx, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }

    const ValidatedMoves *vms = game_event_get_vms(event);
    const Move *actual_move = validated_moves_get_move(vms, 0);
    const Rack *event_rack = game_event_get_const_rack(event);

    TurnResult turn_result = {0};
    turn_result.turn_number = turn_counter;
    turn_result.player_index = player_index;
    turn_result.event_idx = event_idx;
    rack_copy(&turn_result.rack, event_rack);
    turn_result.note_str = game_event_get_note(event);

    char *move_str =
        analyze_format_move(actual_move, game_get_board(ctx->game), ld);
    thread_control_print_formatted(analyze_args->sim_args.thread_control,
                                   "Event %d: %s\n", turn_counter, move_str);
    free(move_str);

    if (analyze_args->sim_args.num_plies == 0) {
      analyze_with_static(event, &turn_result, game_history, ld, report_path,
                          ctx, analyze_args->max_num_display_plays,
                          analyze_args->human_readable, is_first_analyzed_turn,
                          vertical_opening_is_transposable, error_stack);
    } else if (bag_get_letters(game_get_bag(ctx->game)) > 0) {
      analyze_with_sim(event, &turn_result, game_history, ld, report_path, ctx,
                       analyze_args, is_first_analyzed_turn,
                       vertical_opening_is_transposable, error_stack);
    } else {
      analyze_with_endgame(event, &turn_result, game_history, ld, report_path,
                           ctx, analyze_args, is_first_analyzed_turn,
                           error_stack);
    }

    is_first_analyzed_turn = false;

    if (!error_stack_is_empty(error_stack)) {
      break;
    }

    player_turn_counts[player_index]++;
    analyze_ctx_push_turn_result(ctx, &turn_result);

    if (thread_control_get_status(analyze_args->sim_args.thread_control) ==
        THREAD_CONTROL_STATUS_USER_INTERRUPT) {
      break;
    }
  }

  if (error_stack_is_empty(error_stack) && analyze_args->human_readable) {
    // Determine which players appear in the results.
    bool player_has_turns[2] = {false, false};
    for (int result_idx = 0; result_idx < ctx->turn_results_count;
         result_idx++) {
      const int pidx = ctx->turn_results[result_idx].player_index;
      if (pidx == 0 || pidx == 1) {
        player_has_turns[pidx] = true;
      }
    }
    // Write a summary for each player that was analyzed.
    for (int player_idx = 0; player_idx < 2; player_idx++) {
      if (player_has_turns[player_idx]) {
        write_analysis_summary(game_history, ld, ctx->turn_results,
                               ctx->turn_results_count, report_path, player_idx,
                               player_turn_counts[player_idx], ctx->game,
                               error_stack);
        if (!error_stack_is_empty(error_stack)) {
          break;
        }
      }
    }
    // Write combined summary only when both players were analyzed.
    if (error_stack_is_empty(error_stack) && player_has_turns[0] &&
        player_has_turns[1]) {
      write_analysis_summary(game_history, ld, ctx->turn_results,
                             ctx->turn_results_count, report_path, -1,
                             player_turn_counts[0] + player_turn_counts[1],
                             ctx->game, error_stack);
    }
  }
}
