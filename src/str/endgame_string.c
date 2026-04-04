#include "../ent/board.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../impl/gameplay.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// Renders one PV's move sequence inline with end-rack annotations and a
// greedy/exact boundary marker, mirroring the per-PV logic in log_final_pvs.
// gc is consumed (mutated by play_move calls); the caller must not reuse it.
static void string_builder_add_pvline_inline(StringBuilder *sb,
                                             const PVLine *pv, Game *gc,
                                             const LetterDistribution *ld) {
  Move move;
  for (int i = 0; i < pv->num_moves; i++) {
    small_move_to_move(&move, &pv->moves[i], game_get_board(gc));
    string_builder_add_move(sb, game_get_board(gc), &move, ld, true);
    play_move(&move, gc, NULL);
    if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
      const int opp_idx = game_get_player_on_turn_index(gc);
      const Rack *opp_rack = player_get_rack(game_get_player(gc, opp_idx));
      const int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
      string_builder_add_string(sb, " (");
      string_builder_add_rack(sb, opp_rack, ld, false);
      string_builder_add_formatted_string(sb, " +%d)", adj);
    } else if (game_get_game_end_reason(gc) ==
               GAME_END_REASON_CONSECUTIVE_ZEROS) {
      string_builder_add_string(sb, " (6 zeros)");
    }
    if (i < pv->num_moves - 1) {
      if (i + 1 == pv->negamax_depth && pv->negamax_depth > 0) {
        string_builder_add_string(sb, " |");
      }
      string_builder_add_string(sb, " ");
    }
  }
}

void string_builder_endgame_results(StringBuilder *pv_description,
                                    const EndgameResults *endgame_results,
                                    const Game *game,
                                    const GameHistory *game_history,
                                    bool add_line_breaks) {
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

  if (add_line_breaks && num_pvs > 0) {
    // Post-solve display: summary table + per-PV inline sequences.
    string_builder_add_formatted_string(
        pv_description, "Endgame (depth: %d, time: %.3fs)\n", depth,
        endgame_results_get_display_seconds_elapsed(endgame_results));

    const int p1_score =
        equity_to_int(player_get_score(game_get_player(source_game, 0)));
    const int p2_score =
        equity_to_int(player_get_score(game_get_player(source_game, 1)));
    const int on_turn = game_get_player_on_turn_index(source_game);

    // Build summary table: one row per PV.
    StringGrid *sg = string_grid_create(num_pvs + 1, 3, 1);
    string_grid_set_cell(sg, 0, 0, string_duplicate("Play"));
    string_grid_set_cell(sg, 0, 1, string_duplicate("Value"));
    string_grid_set_cell(sg, 0, 2, string_duplicate("Spread"));

    StringBuilder *tmp_sb = string_builder_create();
    for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
      const PVLine *pv =
          endgame_results_get_multi_pvline(endgame_results, pv_idx);
      // Col 0: first move of the PV decoded against a fresh game copy.
      Game *gc = game_duplicate(source_game);
      Move move;
      small_move_to_move(&move, &pv->moves[0], game_get_board(gc));
      string_builder_add_move(tmp_sb, game_get_board(gc), &move, ld, true);
      string_grid_set_cell(sg, pv_idx + 1, 0,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
      game_destroy(gc);

      // Col 1: endgame value (spread delta from on-turn player's perspective).
      string_grid_set_cell(sg, pv_idx + 1, 1,
                           get_formatted_string("%d", pv->score));

      // Col 2: final game spread from P1's perspective.
      const int final_spread =
          (p1_score - p2_score) +
          (on_turn == 0 ? pv->score : -pv->score);
      if (final_spread > 0) {
        string_grid_set_cell(sg, pv_idx + 1, 2,
                             get_formatted_string("P1 +%d", final_spread));
      } else if (final_spread < 0) {
        string_grid_set_cell(sg, pv_idx + 1, 2,
                             get_formatted_string("P2 +%d", -final_spread));
      } else {
        string_grid_set_cell(sg, pv_idx + 1, 2, string_duplicate("Tie"));
      }
    }
    string_builder_destroy(tmp_sb);
    string_builder_add_string_grid(pv_description, sg, false);
    string_grid_destroy(sg);

    // Per-PV inline sequences below the table.
    for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
      const PVLine *pv =
          endgame_results_get_multi_pvline(endgame_results, pv_idx);
      if (num_pvs > 1) {
        string_builder_add_formatted_string(pv_description, "%d: ",
                                            pv_idx + 1);
      }
      Game *gc = game_duplicate(source_game);
      string_builder_add_pvline_inline(pv_description, pv, gc, ld);
      game_destroy(gc);
      string_builder_add_string(pv_description, "\n");
    }
  } else if (add_line_breaks) {
    // Mid-solve fallback: display the single best PV from display_pv_data
    // using the existing move-by-move grid format.
    const PVLine *pv_line =
        endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_DISPLAY);
    const int endgame_value =
        endgame_results_get_value(endgame_results, ENDGAME_RESULT_DISPLAY);
    string_builder_add_formatted_string(
        pv_description,
        "Principal Variation (depth: %d, value: %d, length: %d, time: "
        "%.3fs)\n",
        depth, endgame_value, pv_line->num_moves,
        endgame_results_get_display_seconds_elapsed(endgame_results));

    Game *gc = game_duplicate(source_game);
    const Board *board = game_get_board(gc);
    StringGrid *sg = string_grid_create(pv_line->num_moves, 3, 1);
    StringBuilder *tmp_sb = string_builder_create();
    const int start_player_index = game_get_player_on_turn_index(gc);
    const int start_player_final_score_diff =
        equity_to_int(
            player_get_score(game_get_player(gc, start_player_index)) -
            player_get_score(game_get_player(gc, 1 - start_player_index))) +
        endgame_value;
    const char *start_player_name;
    if (game_history) {
      start_player_name =
          game_history_player_get_name(game_history, start_player_index);
    } else {
      start_player_name = start_player_index == 0 ? PLAYER_ONE_DEFAULT_NAME
                                                  : PLAYER_TWO_DEFAULT_NAME;
    }
    for (int i = 0; i < pv_line->num_moves; i++) {
      int curr_col = 0;
      const int player_on_turn = game_get_player_on_turn_index(gc);
      const char *player_name;
      if (game_history) {
        player_name =
            game_history_player_get_name(game_history, player_on_turn);
      } else {
        player_name = player_on_turn == 0 ? PLAYER_ONE_DEFAULT_NAME
                                          : PLAYER_TWO_DEFAULT_NAME;
      }
      string_grid_set_cell(sg, i, curr_col++,
                           get_formatted_string("(%s)", player_name));
      string_grid_set_cell(sg, i, curr_col++,
                           get_formatted_string("%d:", i + 1));
      Move move;
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(tmp_sb, board, &move, ld, true);
      string_grid_set_cell(sg, i, curr_col++,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
      play_move(&move, gc, NULL);
    }
    string_builder_destroy(tmp_sb);
    string_builder_add_string_grid(pv_description, sg, false);
    string_grid_destroy(sg);
    string_builder_add_formatted_string(pv_description, "\n%s ",
                                        start_player_name);
    if (start_player_final_score_diff > 0) {
      string_builder_add_formatted_string(pv_description, "wins by %d.\n",
                                          start_player_final_score_diff);
    } else if (start_player_final_score_diff < 0) {
      string_builder_add_formatted_string(pv_description, "loses by %d.\n",
                                          -start_player_final_score_diff);
    } else {
      string_builder_add_string(pv_description, "ties.\n");
    }
    game_destroy(gc);
  } else {
    // Machine-readable: compact single best PV inline.
    const PVLine *pv_line =
        endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_DISPLAY);
    const int endgame_value =
        endgame_results_get_value(endgame_results, ENDGAME_RESULT_DISPLAY);
    string_builder_add_formatted_string(
        pv_description,
        "Principal Variation (depth: %d, value: %d, length: %d, time: "
        "%.3fs) ",
        depth, endgame_value, pv_line->num_moves,
        endgame_results_get_display_seconds_elapsed(endgame_results));

    Game *gc = game_duplicate(source_game);
    const Board *board = game_get_board(gc);
    for (int i = 0; i < pv_line->num_moves; i++) {
      string_builder_add_formatted_string(pv_description, "%d: ", i + 1);
      Move move;
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(pv_description, board, &move, ld, true);
      if (i != pv_line->num_moves - 1) {
        string_builder_add_string(pv_description, " -> ");
      }
      play_move(&move, gc, NULL);
    }
    string_builder_add_string(pv_description, "\n");
    game_destroy(gc);
  }
}

char *endgame_results_get_string(EndgameResults *endgame_results,
                                 const Game *game,
                                 const GameHistory *game_history,
                                 bool add_line_breaks) {
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
                                 game_history, add_line_breaks);
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_DISPLAY);
  char *pvline_string = string_builder_dump(pv_description, NULL);
  string_builder_destroy(pv_description);
  return pvline_string;
}