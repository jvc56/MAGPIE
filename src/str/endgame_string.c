#include "../def/game_defs.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/transposition_table.h"
#include "../impl/endgame.h"
#include "../impl/gameplay.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// Formats a single PVLine into sb as a per-move table with columns:
// Player, Move, Score, Value, Spread. A "---" separator row is inserted
// between the exact (negamax) moves and the greedy continuation moves.
// When num_pvs == 0 (solve in progress), pv_idx is ignored and the current
// best display PV (TT-extended) is shown. When num_pvs > 0 (solve complete),
// pv_idx selects which multi-PV line to display.
// Assumes the display lock is already held by the caller.
static void string_builder_endgame_single_pv_with_lock(
    StringBuilder *sb, EndgameResults *endgame_results, const Game *source_game,
    const GameHistory *game_history, int pv_idx) {
  PVLine pv;
  int solving_player;
  int endgame_value;
  int depth;

  const int num_pvs = endgame_results_get_num_pvs(endgame_results);
  if (num_pvs == 0) {
    // Solve in progress: extend the current best PV from the TT for display.
    const PVLine *raw_pv =
        endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_DISPLAY);
    endgame_value =
        endgame_results_get_value(endgame_results, ENDGAME_RESULT_DISPLAY);
    depth = endgame_results_get_depth(endgame_results, ENDGAME_RESULT_DISPLAY);
    solving_player = endgame_results_get_solving_player(endgame_results);

    pv = *raw_pv;
    TranspositionTable *tt = endgame_results_get_tt(endgame_results);
    const int max_depth = endgame_results_get_max_depth(endgame_results);
    Game *ext_game =
        endgame_results_prepare_ext_game(endgame_results, source_game);
    pvline_extend_from_tt(&pv, ext_game, tt, solving_player, max_depth, 0,
                          ENDGAME_MOVEGEN_RESULT_DISPLAY);
  } else {
    // Solve complete: display the requested PV directly.
    pv = *endgame_results_get_multi_pvline(endgame_results, pv_idx);
    // Use the PV's own negamax_depth as the exact-search depth for this line.
    depth = pv.negamax_depth;
    solving_player = game_get_player_on_turn_index(source_game);
    endgame_value = pv.score;
  }

  const int p0_score =
      equity_to_int(player_get_score(game_get_player(source_game, 0)));
  const int p1_score =
      equity_to_int(player_get_score(game_get_player(source_game, 1)));
  const int initial_on_turn_spread =
      (solving_player == 0) ? p0_score - p1_score : p1_score - p0_score;
  const int final_spread = initial_on_turn_spread + endgame_value;

  string_builder_add_formatted_string(
      sb, "PV %d (spread: %d, value: %d, depth: %d, length: %d, time: %.3fs)\n",
      pv_idx + 1, final_spread, endgame_value, depth, pv.num_moves,
      endgame_results_get_seconds_elapsed(endgame_results));

  if (pv.num_moves == 0) {
    return;
  }

  const LetterDistribution *ld = game_get_ld(source_game);
  const int p0_initial =
      equity_to_int(player_get_score(game_get_player(source_game, 0)));
  const int p1_initial =
      equity_to_int(player_get_score(game_get_player(source_game, 1)));
  const int initial_spread =
      (solving_player == 0) ? p0_initial - p1_initial : p1_initial - p0_initial;

  // A separator row of dashes is inserted between the exact (negamax) moves
  // and the greedy continuation moves.
  const bool has_separator =
      (pv.negamax_depth > 0 && pv.negamax_depth < pv.num_moves);
  const int num_rows = pv.num_moves + 1 + (has_separator ? 1 : 0);
  StringGrid *sg = string_grid_create(num_rows, 5, 1);
  string_grid_set_cell(sg, 0, 0, string_duplicate("Player"));
  string_grid_set_cell(sg, 0, 1, string_duplicate("Move"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("Score"));
  string_grid_set_cell(sg, 0, 3, string_duplicate("Value"));
  string_grid_set_cell(sg, 0, 4, string_duplicate("Spread"));

  Game *gc = game_duplicate(source_game);
  StringBuilder *tmp_sb = string_builder_create();
  for (int move_idx = 0; move_idx < pv.num_moves; move_idx++) {
    // Insert separator row of dashes after the last exact (negamax) move.
    if (has_separator && move_idx == pv.negamax_depth) {
      const int sep_row = pv.negamax_depth + 1;
      for (int col_idx = 0; col_idx < 5; col_idx++) {
        string_grid_set_cell(sg, sep_row, col_idx, string_duplicate("---"));
      }
    }
    // Rows after the negamax boundary are shifted down by the separator row.
    const int grid_row = (has_separator && move_idx >= pv.negamax_depth)
                             ? move_idx + 2
                             : move_idx + 1;
    const int player_idx = game_get_player_on_turn_index(gc);

    // Player column: nickname if available, otherwise numeric index.
    const char *nickname = NULL;
    if (game_history) {
      nickname = game_history_player_get_nickname(game_history, player_idx);
    }
    if (nickname && nickname[0] != '\0') {
      string_grid_set_cell(sg, grid_row, 0, string_duplicate(nickname));
    } else {
      string_grid_set_cell(sg, grid_row, 0,
                           get_formatted_string("%d", player_idx));
    }

    // Move column: move string with optional end-rack annotation.
    Move move;
    small_move_to_move(&move, &pv.moves[move_idx], game_get_board(gc));
    string_builder_add_move(tmp_sb, game_get_board(gc), &move, ld, true);
    play_move(&move, gc, NULL);

    if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
      const int opp_idx = game_get_player_on_turn_index(gc);
      const Rack *opp_rack = player_get_rack(game_get_player(gc, opp_idx));
      const int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
      string_builder_add_string(tmp_sb, " (");
      string_builder_add_rack(tmp_sb, opp_rack, ld, false);
      string_builder_add_formatted_string(tmp_sb, " +%d)", adj);
    } else if (game_get_game_end_reason(gc) ==
               GAME_END_REASON_CONSECUTIVE_ZEROS) {
      string_builder_add_string(tmp_sb, " (6 zeros)");
    }
    string_grid_set_cell(sg, grid_row, 1, string_builder_dump(tmp_sb, NULL));
    string_builder_clear(tmp_sb);

    // Score column: the move's individual point value.
    const int move_score = (int)small_move_get_score(&pv.moves[move_idx]);
    string_grid_set_cell(sg, grid_row, 2,
                         get_formatted_string("%d", move_score));

    // Value and Spread columns computed from live game state after the move.
    const int p0 = equity_to_int(player_get_score(game_get_player(gc, 0)));
    const int p1 = equity_to_int(player_get_score(game_get_player(gc, 1)));
    const int spread_after = (solving_player == 0) ? p0 - p1 : p1 - p0;
    const int value_after = spread_after - initial_spread;
    string_grid_set_cell(sg, grid_row, 3,
                         get_formatted_string("%d", value_after));
    string_grid_set_cell(sg, grid_row, 4,
                         get_formatted_string("%d", spread_after));
  }
  string_builder_destroy(tmp_sb);
  game_destroy(gc);

  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
}

void string_builder_endgame_single_pv(StringBuilder *sb,
                                      EndgameResults *endgame_results,
                                      const Game *source_game,
                                      const GameHistory *game_history,
                                      int pv_idx) {
  endgame_results_lock(endgame_results, ENDGAME_RESULT_DISPLAY);
  endgame_results_lock(endgame_results, ENDGAME_RESULT_BEST);
  endgame_results_update_display_data(endgame_results);
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_BEST);
  string_builder_endgame_single_pv_with_lock(sb, endgame_results, source_game,
                                             game_history, pv_idx);
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_DISPLAY);
}

static void string_builder_endgame_results(StringBuilder *pv_description,
                                           EndgameResults *endgame_results,
                                           const Game *game,
                                           const GameHistory *game_history) {
  const int depth =
      endgame_results_get_depth(endgame_results, ENDGAME_RESULT_DISPLAY);
  if (depth < 0) {
    string_builder_add_string(pv_description,
                              "No principal variation available.\n");
    return;
  }

  // Prefer the start_game snapshot (captured when the solve began) so that
  // the PV can be decoded correctly even if config->game has since changed
  // (e.g. after newgame).
  const Game *source_game = endgame_results_get_start_game(endgame_results);
  if (!source_game) {
    source_game = game;
  }
  const LetterDistribution *ld = game_get_ld(source_game);
  const int num_pvs = endgame_results_get_num_pvs(endgame_results);

  // num_pvs == 0: a solve is in progress; show the current best PV with TT
  // extension in a per-move table for live display.
  if (num_pvs == 0) {
    string_builder_endgame_single_pv_with_lock(pv_description, endgame_results,
                                               source_game, game_history, 0);
    return;
  }

  // num_pvs > 0: solve is complete; multi_pvs are already fully extended by
  // endgame_solve, so display them directly in a StringGrid without
  // re-extending.
  string_builder_add_formatted_string(
      pv_description, "Endgame (depth: %d, time: %.3fs)\n", depth,
      endgame_results_get_seconds_elapsed(endgame_results));

  const int p1_score =
      equity_to_int(player_get_score(game_get_player(source_game, 0)));
  const int p2_score =
      equity_to_int(player_get_score(game_get_player(source_game, 1)));
  const int on_turn = game_get_player_on_turn_index(source_game);

  // Find the maximum sequence length across all PVs.
  int max_seq_len = 0;
  for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
    const PVLine *pv =
        endgame_results_get_multi_pvline(endgame_results, pv_idx);
    if (pv->num_moves > max_seq_len) {
      max_seq_len = pv->num_moves;
    }
  }

  // Build grid: Play, Value, Spread, [Turn 2, Turn 3, ..., Turn N]
  // Col 0 = Play (first move), cols 1-2 = Value/Spread,
  // col (turn_idx + 1) = Turn turn_idx for turn_idx in [2..max_seq_len].
  const int num_cols = 3 + (max_seq_len > 1 ? max_seq_len - 1 : 0);
  StringGrid *sg = string_grid_create(num_pvs + 1, num_cols, 1);
  string_grid_set_cell(sg, 0, 0, string_duplicate("Play"));
  string_grid_set_cell(sg, 0, 1, string_duplicate("V"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("S"));
  for (int turn_idx = 2; turn_idx <= max_seq_len; turn_idx++) {
    string_grid_set_cell(sg, 0, turn_idx + 1,
                         get_formatted_string("Turn %d", turn_idx));
  }

  StringBuilder *tmp_sb = string_builder_create();
  for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
    const PVLine *pv =
        endgame_results_get_multi_pvline(endgame_results, pv_idx);

    Game *gc = game_duplicate(source_game);
    for (int move_idx = 0; move_idx < pv->num_moves; move_idx++) {
      Move move;
      small_move_to_move(&move, &pv->moves[move_idx], game_get_board(gc));

      // Prepend "| " to the first greedy move to mark the solved/greedy
      // boundary.
      if (move_idx == pv->negamax_depth && pv->negamax_depth > 0) {
        string_builder_add_string(tmp_sb, "| ");
      }
      string_builder_add_move(tmp_sb, game_get_board(gc), &move, ld, true);
      play_move(&move, gc, NULL);

      // Annotate the move that ends the game.
      if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
        const int opp_idx = game_get_player_on_turn_index(gc);
        const Rack *opp_rack = player_get_rack(game_get_player(gc, opp_idx));
        const int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
        string_builder_add_string(tmp_sb, " (");
        string_builder_add_rack(tmp_sb, opp_rack, ld, false);
        string_builder_add_formatted_string(tmp_sb, " +%d)", adj);
      } else if (game_get_game_end_reason(gc) ==
                 GAME_END_REASON_CONSECUTIVE_ZEROS) {
        string_builder_add_string(tmp_sb, " (6 zeros)");
      }
      if (move_idx < pv->num_moves - 1) {
        string_builder_add_string(tmp_sb, " ");
      }

      // move_idx 0 → col 0 (Play); move_idx k ≥ 1 → col k+2 (Turn k+1).
      const int col = (move_idx == 0) ? 0 : (move_idx + 2);
      string_grid_set_cell(sg, pv_idx + 1, col,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
    }
    game_destroy(gc);

    string_grid_set_cell(sg, pv_idx + 1, 1,
                         get_formatted_string("%d", pv->score));

    const int on_turn_spread =
        (on_turn == 0 ? p1_score - p2_score : p2_score - p1_score) + pv->score;
    string_grid_set_cell(sg, pv_idx + 1, 2,
                         get_formatted_string("%d", on_turn_spread));
  }
  string_builder_destroy(tmp_sb);
  string_builder_add_string_grid(pv_description, sg, false);
  string_grid_destroy(sg);
}

char *endgame_results_get_string(EndgameResults *endgame_results,
                                 const Game *game,
                                 const GameHistory *game_history) {
  StringBuilder *pv_description = string_builder_create();

  // Lock the display mutex so endgame prints cannot overwrite each other
  endgame_results_lock(endgame_results, ENDGAME_RESULT_DISPLAY);
  endgame_results_lock(endgame_results, ENDGAME_RESULT_BEST);

  // Copy current endgame_results to display endgame_results
  endgame_results_update_display_data(endgame_results);

  // Immediately unlock the best pv data mutex so the endgame solver can
  // continue
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_BEST);

  // Maintain the display lock until we finish generating the string
  string_builder_endgame_results(pv_description, endgame_results, game,
                                 game_history);
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_DISPLAY);
  char *pvline_string = string_builder_dump(pv_description, NULL);
  string_builder_destroy(pv_description);
  return pvline_string;
}
