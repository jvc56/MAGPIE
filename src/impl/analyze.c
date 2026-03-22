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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ANALYZE_NOTE_TRUNCATE_LEN = 30,
  ANALYZE_SUMMARY_COLS = 8,
  ANALYZE_SUMMARY_COL_PADDING = 2,
  ANALYZE_NUM_TOP_ENDGAME_MOVES = 1,
};

typedef struct TurnResult {
  int turn_number;
  int player_index;
  char *rack_str;
  char *actual_play_str;
  char *best_play_str; // NULL if actual is the best play
  Equity equity_lost;  // millipoints; 0 if actual is best
  double win_pct_lost; // 0.0-100.0; 0.0 if actual is best
  char *note_str;      // NULL if event has no note
  bool was_endgame;
} TurnResult;

typedef struct AnalyzeCtx {
  Game *game;
  MoveList *move_list;
  SimResults *sim_results;
  EndgameResults *endgame_results;
  EndgameSolver *endgame_solver;
  InferenceResults *inference_results;
  SimCtx *sim_ctx; // NULL initially; persisted across sim calls
} AnalyzeCtx;

// File-scope static instance — reused across games in directory mode
static AnalyzeCtx analyze_ctx = {0};

static void analyze_ctx_ensure_created(const GameArgs *game_args,
                                       int num_plays) {
  if (!analyze_ctx.game) {
    analyze_ctx.game = game_create(game_args);
    analyze_ctx.move_list = move_list_create(num_plays);
    analyze_ctx.sim_results = sim_results_create(0.0);
    analyze_ctx.endgame_results = endgame_results_create();
    analyze_ctx.endgame_solver = endgame_solver_create();
    analyze_ctx.inference_results = inference_results_create(NULL);
    analyze_ctx.sim_ctx = NULL;
  } else {
    game_update(analyze_ctx.game, game_args);
    move_list_resize(analyze_ctx.move_list, num_plays);
  }
}

static char *build_rack_str(const Rack *rack, const LetterDistribution *ld) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, rack, ld, false);
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}

static char *build_move_description_str(const Move *move,
                                        const LetterDistribution *ld) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move_description(sb, move, ld);
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}

// Writes per-turn human-readable section to report_file.
static void write_per_turn_human_readable(const TurnResult *turn_result,
                                          const GameHistory *game_history,
                                          FILE *report_file,
                                          bool is_sim,
                                          int max_num_display_plays) {
  const char *player_name = game_history_player_get_nickname(
      game_history, turn_result->player_index);

  fprintf(report_file,
          "\n--- Turn %d: %s (Rack: %s) ---\n"
          "Actual: %s\n",
          turn_result->turn_number, player_name,
          turn_result->rack_str ? turn_result->rack_str : "",
          turn_result->actual_play_str ? turn_result->actual_play_str : "");

  if (is_sim) {
    char *sim_str = sim_results_get_string(
        analyze_ctx.game, analyze_ctx.sim_results, max_num_display_plays,
        2, -1, -1, NULL, 0, false, false, NULL);
    if (sim_str) {
      fprintf(report_file, "%s\n", sim_str);
      free(sim_str);
    }
  } else {
    char *endgame_str = endgame_results_get_string(
        analyze_ctx.endgame_results, analyze_ctx.game, game_history, true);
    if (endgame_str) {
      fprintf(report_file, "%s\n", endgame_str);
      free(endgame_str);
    }
  }

  fprintf(report_file, "Equity lost: %.2f\n",
          equity_to_double(turn_result->equity_lost));
  fprintf(report_file, "Win%% lost: %.2f\n", turn_result->win_pct_lost);

  if (turn_result->note_str) {
    fprintf(report_file, "Note: %s\n", turn_result->note_str);
  }
}

// Writes per-turn CSV row to report_file.
static void write_per_turn_csv(const TurnResult *turn_result,
                                const GameHistory *game_history,
                                FILE *report_file,
                                bool is_first_turn) {
  if (is_first_turn) {
    fprintf(report_file,
            "turn,player,rack,actual,best,equity_lost,win_pct_lost\n");
  }

  const char *player_name = game_history_player_get_nickname(
      game_history, turn_result->player_index);
  const char *best = turn_result->best_play_str ? turn_result->best_play_str
                                                 : "-";

  fprintf(report_file, "%d,%s,%s,%s,%s,%.2f,%.2f\n",
          turn_result->turn_number, player_name,
          turn_result->rack_str ? turn_result->rack_str : "",
          turn_result->actual_play_str ? turn_result->actual_play_str : "",
          best,
          equity_to_double(turn_result->equity_lost),
          turn_result->win_pct_lost);
}

static void analyze_with_sim(const GameEvent *event, TurnResult *turn_result,
                              const GameHistory *game_history,
                              FILE *report_file, WinPct *win_pcts,
                              ThreadControl *thread_control, int num_plays,
                              int sim_plies, int num_threads,
                              double stop_cond_pct, bool sim_with_inference,
                              int max_num_display_plays, bool human_readable,
                              bool is_first_analyzed_turn,
                              ErrorStack *error_stack) {
  // Generate all moves (leave as min-heap; do not sort)
  move_list_reset(analyze_ctx.move_list);
  const MoveGenArgs gen_args = {
      .game = analyze_ctx.game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = analyze_ctx.move_list,
  };
  generate_moves(&gen_args);

  // Get actual move from the game event
  ValidatedMoves *vms = game_event_get_vms(event);
  const Move *actual_move = validated_moves_get_move(vms, 0);

  // Search the min-heap for the actual move
  int actual_idx = -1;
  int num_moves = move_list_get_count(analyze_ctx.move_list);
  for (int sp_idx = 0; sp_idx < num_moves; sp_idx++) {
    if (compare_moves_without_equity(
            move_list_get_move(analyze_ctx.move_list, sp_idx), actual_move,
            true) == -1) {
      actual_idx = sp_idx;
      break;
    }
  }

  if (actual_idx == -1) {
    // Actual move was not in the generated list (e.g., phony); insert it
    move_list_add_move(analyze_ctx.move_list, actual_move);
    num_moves = move_list_get_count(analyze_ctx.move_list);
    for (int sp_idx = 0; sp_idx < num_moves; sp_idx++) {
      if (compare_moves_without_equity(
              move_list_get_move(analyze_ctx.move_list, sp_idx), actual_move,
              true) == -1) {
        actual_idx = sp_idx;
        break;
      }
    }
  }

  // Use the opponent's current rack as the known opp rack
  const int player_on_turn_index =
      game_get_player_on_turn_index(analyze_ctx.game);
  Rack *known_opp_rack = player_get_rack(
      game_get_player(analyze_ctx.game, 1 - player_on_turn_index));

  // Build sim args and run simulation
  SimArgs sim_args;
  sim_args_fill(sim_plies, analyze_ctx.move_list, num_plays, known_opp_rack,
                win_pcts, analyze_ctx.inference_results, thread_control,
                analyze_ctx.game, sim_with_inference, false, num_threads,
                0, max_num_display_plays, 2, 0, (uint64_t)num_plays, 1,
                stop_cond_pct, BAI_THRESHOLD_NONE, 0,
                BAI_SAMPLING_RULE_ROUND_ROBIN, 0.0, NULL, &sim_args);

  simulate(&sim_args, &analyze_ctx.sim_ctx, analyze_ctx.sim_results,
           error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Find the best simmed play and the actual simmed play
  const Move *best_move = sim_results_get_best_move(analyze_ctx.sim_results);
  const SimmedPlay *best_sp = NULL;
  const SimmedPlay *actual_sp = NULL;
  const int num_simmed =
      sim_results_get_number_of_plays(analyze_ctx.sim_results);

  for (int sp_idx = 0; sp_idx < num_simmed; sp_idx++) {
    const SimmedPlay *sp =
        sim_results_get_simmed_play(analyze_ctx.sim_results, sp_idx);
    const Move *sp_move = simmed_play_get_move(sp);
    if (best_move != NULL &&
        compare_moves_without_equity(sp_move, best_move, true) == -1) {
      best_sp = sp;
    }
    if (compare_moves_without_equity(sp_move, actual_move, true) == -1) {
      actual_sp = sp;
    }
  }

  // Compute equity and win% lost
  if (best_sp == NULL || actual_sp == NULL || best_sp == actual_sp) {
    turn_result->equity_lost = 0;
    turn_result->win_pct_lost = 0.0;
    turn_result->best_play_str = NULL;
  } else {
    const double best_equity =
        stat_get_mean(simmed_play_get_equity_stat(best_sp));
    const double actual_equity =
        stat_get_mean(simmed_play_get_equity_stat(actual_sp));
    turn_result->equity_lost = double_to_equity(best_equity - actual_equity);
    const double best_win_pct =
        stat_get_mean(simmed_play_get_win_pct_stat(best_sp));
    const double actual_win_pct =
        stat_get_mean(simmed_play_get_win_pct_stat(actual_sp));
    turn_result->win_pct_lost = (best_win_pct - actual_win_pct) * 100.0;
    turn_result->best_play_str = build_move_description_str(
        simmed_play_get_move(best_sp), game_get_ld(analyze_ctx.game));
  }

  turn_result->was_endgame = false;

  if (human_readable) {
    write_per_turn_human_readable(turn_result, game_history, report_file,
                                  true, max_num_display_plays);
  } else {
    write_per_turn_csv(turn_result, game_history, report_file,
                       is_first_analyzed_turn);
  }
}

static void analyze_with_endgame(const GameEvent *event,
                                 TurnResult *turn_result,
                                 const GameHistory *game_history,
                                 FILE *report_file,
                                 ThreadControl *thread_control,
                                 int endgame_plies,
                                 double tt_fraction_of_mem, int num_threads,
                                 bool human_readable,
                                 bool is_first_analyzed_turn,
                                 ErrorStack *error_stack) {
  // Run endgame solver from current position
  EndgameArgs endgame_args;
  memset(&endgame_args, 0, sizeof(endgame_args));
  endgame_args.thread_control = thread_control;
  endgame_args.game = analyze_ctx.game;
  endgame_args.plies = endgame_plies;
  endgame_args.tt_fraction_of_mem = tt_fraction_of_mem;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = num_threads;
  endgame_args.num_top_moves = ANALYZE_NUM_TOP_ENDGAME_MOVES;
  endgame_args.use_heuristics = true;

  endgame_solve(analyze_ctx.endgame_solver, &endgame_args,
                analyze_ctx.endgame_results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const int v_best =
      endgame_results_get_value(analyze_ctx.endgame_results, ENDGAME_RESULT_BEST);
  const PVLine *best_pvline =
      endgame_results_get_pvline(analyze_ctx.endgame_results, ENDGAME_RESULT_BEST);

  ValidatedMoves *vms = game_event_get_vms(event);
  const Move *actual_move = validated_moves_get_move(vms, 0);

  // Convert best SmallMove to Move for comparison
  Move best_first_move;
  bool actual_is_best = false;
  if (best_pvline != NULL && best_pvline->num_moves > 0) {
    small_move_to_move(&best_first_move, &best_pvline->moves[0],
                       game_get_board(analyze_ctx.game));
    actual_is_best =
        compare_moves_without_equity(&best_first_move, actual_move, true) ==
        -1;
  } else {
    actual_is_best = true;
  }

  if (actual_is_best) {
    turn_result->equity_lost = 0;
    turn_result->win_pct_lost = 0.0;
    turn_result->best_play_str = NULL;
  } else {
    // Play the actual move on a copy and re-solve from opponent's perspective
    Game *game_copy = game_duplicate(analyze_ctx.game);
    play_move(actual_move, game_copy, NULL);

    EndgameArgs endgame_args_copy;
    memset(&endgame_args_copy, 0, sizeof(endgame_args_copy));
    endgame_args_copy.thread_control = thread_control;
    endgame_args_copy.game = game_copy;
    endgame_args_copy.plies = endgame_plies;
    endgame_args_copy.tt_fraction_of_mem = tt_fraction_of_mem;
    endgame_args_copy.initial_small_move_arena_size =
        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    endgame_args_copy.num_threads = num_threads;
    endgame_args_copy.num_top_moves = ANALYZE_NUM_TOP_ENDGAME_MOVES;
    endgame_args_copy.use_heuristics = true;

    endgame_results_reset(analyze_ctx.endgame_results);
    endgame_solve(analyze_ctx.endgame_solver, &endgame_args_copy,
                  analyze_ctx.endgame_results, error_stack);
    game_destroy(game_copy);

    if (!error_stack_is_empty(error_stack)) {
      return;
    }

    // v_actual_opp is the opponent's value after our actual move;
    // from our perspective that is negated.
    const int v_actual_opp =
        endgame_results_get_value(analyze_ctx.endgame_results,
                                  ENDGAME_RESULT_BEST);
    const int v_actual = -v_actual_opp;

    turn_result->equity_lost = int_to_equity(v_best - v_actual);
    // Win% lost: 100% if best move wins but actual move loses, else 0%
    turn_result->win_pct_lost =
        (v_best >= 0 && v_actual < 0) ? 100.0 : 0.0;
    turn_result->best_play_str = build_move_description_str(
        &best_first_move, game_get_ld(analyze_ctx.game));

    // Re-solve from original position for display
    endgame_results_reset(analyze_ctx.endgame_results);
    endgame_solve(analyze_ctx.endgame_solver, &endgame_args,
                  analyze_ctx.endgame_results, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  turn_result->was_endgame = true;

  if (human_readable) {
    write_per_turn_human_readable(turn_result, game_history, report_file,
                                  false, 0);
  } else {
    write_per_turn_csv(turn_result, game_history, report_file,
                       is_first_analyzed_turn);
  }
}

static void write_analysis_summary(const GameHistory *game_history,
                                   const TurnResult *turn_results,
                                   int num_turn_results, FILE *report_file) {
  if (num_turn_results == 0) {
    return;
  }

  // Header row + one row per result
  const int num_rows = num_turn_results + 1;
  StringGrid *sg =
      string_grid_create(num_rows, ANALYZE_SUMMARY_COLS,
                         ANALYZE_SUMMARY_COL_PADDING);

  // Header row
  string_grid_set_cell(sg, 0, 0, string_duplicate("Turn"));
  string_grid_set_cell(sg, 0, 1, string_duplicate("Player"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("Rack"));
  string_grid_set_cell(sg, 0, 3, string_duplicate("Actual"));
  string_grid_set_cell(sg, 0, 4, string_duplicate("Best"));
  string_grid_set_cell(sg, 0, 5, string_duplicate("Win%Diff"));
  string_grid_set_cell(sg, 0, 6, string_duplicate("EqDiff"));
  string_grid_set_cell(sg, 0, 7, string_duplicate("Note"));

  Equity total_equity_lost = 0;
  double total_win_pct_lost = 0.0;

  for (int result_idx = 0; result_idx < num_turn_results; result_idx++) {
    const TurnResult *tr = &turn_results[result_idx];
    const int row = result_idx + 1;

    string_grid_set_cell(sg, row, 0,
                         get_formatted_string("%d", tr->turn_number));
    const char *player_name =
        game_history_player_get_nickname(game_history, tr->player_index);
    string_grid_set_cell(sg, row, 1, string_duplicate(player_name));
    string_grid_set_cell(
        sg, row, 2,
        string_duplicate(tr->rack_str ? tr->rack_str : ""));
    string_grid_set_cell(
        sg, row, 3,
        string_duplicate(tr->actual_play_str ? tr->actual_play_str : ""));

    if (tr->best_play_str) {
      string_grid_set_cell(sg, row, 4,
                           string_duplicate(tr->best_play_str));
    } else {
      string_grid_set_cell(sg, row, 4, string_duplicate("-"));
    }

    string_grid_set_cell(sg, row, 5,
                         get_formatted_string("%.2f", tr->win_pct_lost));
    string_grid_set_cell(
        sg, row, 6,
        get_formatted_string("%.2f", equity_to_double(tr->equity_lost)));

    // Truncate note to ANALYZE_NOTE_TRUNCATE_LEN chars
    if (tr->note_str) {
      const size_t note_len = string_length(tr->note_str);
      if ((int)note_len > ANALYZE_NOTE_TRUNCATE_LEN) {
        char *truncated =
            get_substring(tr->note_str, 0, ANALYZE_NOTE_TRUNCATE_LEN);
        char *with_ellipsis =
            get_formatted_string("%s...", truncated);
        free(truncated);
        string_grid_set_cell(sg, row, 7, with_ellipsis);
      } else {
        string_grid_set_cell(sg, row, 7, string_duplicate(tr->note_str));
      }
    } else {
      string_grid_set_cell(sg, row, 7, string_duplicate(""));
    }

    total_equity_lost += tr->equity_lost;
    total_win_pct_lost += tr->win_pct_lost;
  }

  // Print summary table
  StringBuilder *sb = string_builder_create();
  fprintf(report_file, "\n=== Game Summary ===\n");
  string_builder_add_string_grid(sb, sg, true);
  const char *grid_str = string_builder_peek(sb);
  fprintf(report_file, "%s\n", grid_str);
  string_builder_destroy(sb);
  string_grid_destroy(sg);

  // Print aggregate stats
  const double avg_equity =
      equity_to_double(total_equity_lost) / (double)num_turn_results;
  const double avg_win_pct = total_win_pct_lost / (double)num_turn_results;

  fprintf(report_file,
          "Total equity lost: %.2f\n"
          "Average equity lost/turn: %.2f\n"
          "Total win%% lost: %.2f\n"
          "Average win%% lost/turn: %.2f\n",
          equity_to_double(total_equity_lost), avg_equity,
          total_win_pct_lost, avg_win_pct);
}

void analyze_game(GameHistory *game_history, const GameArgs *game_args,
                  WinPct *win_pcts, ThreadControl *thread_control,
                  int player_mask, int num_threads, int num_plays,
                  int sim_plies, int endgame_plies,
                  double tt_fraction_of_mem, double stop_cond_pct,
                  bool sim_with_inference, bool human_readable,
                  int max_num_display_plays, const char *report_path,
                  ErrorStack *error_stack) {
  analyze_ctx_ensure_created(game_args, num_plays);

  FILE *report_file = fopen(report_path, "w");
  if (!report_file) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
        get_formatted_string("cannot open report file: %s", report_path));
    return;
  }

  const int num_events = game_history_get_num_events(game_history);
  TurnResult *turn_results =
      (TurnResult *)calloc_or_die((size_t)num_events, sizeof(TurnResult));
  int num_turn_results = 0;
  int turn_counter = 0;
  bool is_first_analyzed_turn = true;

  for (int event_idx = 0; event_idx < num_events; event_idx++) {
    const GameEvent *event =
        game_history_get_event(game_history, event_idx);
    const game_event_t event_type = game_event_get_type(event);

    // Only analyze player moves (tile placement, exchange, pass)
    if (event_type != GAME_EVENT_TILE_PLACEMENT_MOVE &&
        event_type != GAME_EVENT_EXCHANGE &&
        event_type != GAME_EVENT_PASS) {
      continue;
    }

    turn_counter++;
    const int player_index = game_event_get_player_index(event);

    // Apply player mask: skip if this player is filtered out
    if (player_mask != 0 && !(player_mask & (1 << player_index))) {
      continue;
    }

    // Position game just before this event
    game_play_n_events(game_history, analyze_ctx.game, event_idx, false,
                       error_stack);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }

    // Check endgame condition: are all opponent tiles known?
    const Player *opp_player =
        game_get_player(analyze_ctx.game, 1 - player_index);
    const int unseen_tiles =
        bag_get_letters(game_get_bag(analyze_ctx.game)) +
        (int)rack_get_total_letters(player_get_rack(opp_player));

    // Populate basic turn result fields
    TurnResult turn_result = {0};
    turn_result.turn_number = turn_counter;
    turn_result.player_index = player_index;
    const char *cgp_move_str = game_event_get_cgp_move_string(event);
    turn_result.actual_play_str =
        cgp_move_str ? string_duplicate(cgp_move_str) : string_duplicate("");
    const Rack *event_rack = game_event_get_const_rack(event);
    turn_result.rack_str =
        build_rack_str(event_rack, game_get_ld(analyze_ctx.game));
    const char *note = game_event_get_note(event);
    turn_result.note_str = note ? string_duplicate(note) : NULL;

    if (unseen_tiles > 0) {
      analyze_with_sim(event, &turn_result, game_history, report_file,
                       win_pcts, thread_control, num_plays, sim_plies,
                       num_threads, stop_cond_pct, sim_with_inference,
                       max_num_display_plays, human_readable,
                       is_first_analyzed_turn, error_stack);
    } else {
      analyze_with_endgame(event, &turn_result, game_history, report_file,
                           thread_control, endgame_plies, tt_fraction_of_mem,
                           num_threads, human_readable, is_first_analyzed_turn,
                           error_stack);
    }

    is_first_analyzed_turn = false;

    if (!error_stack_is_empty(error_stack)) {
      free(turn_result.rack_str);
      free(turn_result.actual_play_str);
      free(turn_result.best_play_str);
      free(turn_result.note_str);
      break;
    }

    turn_results[num_turn_results++] = turn_result;
  }

  if (error_stack_is_empty(error_stack) && human_readable) {
    write_analysis_summary(game_history, turn_results, num_turn_results,
                           report_file);
  }

  for (int result_idx = 0; result_idx < num_turn_results; result_idx++) {
    free(turn_results[result_idx].rack_str);
    free(turn_results[result_idx].actual_play_str);
    free(turn_results[result_idx].best_play_str);
    free(turn_results[result_idx].note_str);
  }
  free(turn_results);
  fclose(report_file);
}
