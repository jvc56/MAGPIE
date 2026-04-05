#include "../ent/board.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
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

void string_builder_endgame_results(StringBuilder *pv_description,
                                    const EndgameResults *endgame_results,
                                    const Game *game) {
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

  if (num_pvs > 0) {
    string_builder_add_formatted_string(
        pv_description, "Endgame (depth: %d, time: %.3fs)\n", depth,
        endgame_results_get_display_seconds_elapsed(endgame_results));
  } else {
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
  }

  const int display_pvs = num_pvs > 0 ? num_pvs : 1;

  // Find the maximum sequence length across all displayed PVs.
  int max_seq_len = 0;
  for (int pv_idx = 0; pv_idx < display_pvs; pv_idx++) {
    const PVLine *pv =
        (num_pvs > 0)
            ? endgame_results_get_multi_pvline(endgame_results, pv_idx)
            : endgame_results_get_pvline(endgame_results,
                                        ENDGAME_RESULT_DISPLAY);
    if (pv->num_moves > max_seq_len) {
      max_seq_len = pv->num_moves;
    }
  }

  const int p1_score =
      equity_to_int(player_get_score(game_get_player(source_game, 0)));
  const int p2_score =
      equity_to_int(player_get_score(game_get_player(source_game, 1)));
  const int on_turn = game_get_player_on_turn_index(source_game);

  // Build grid: Play, Value, Spread, [Turn 2, Turn 3, ..., Turn N]
  // Col 0 = Play (first move), cols 1-2 = Value/Spread,
  // col (turn_idx + 1) = Turn turn_idx for turn_idx in [2..max_seq_len].
  const int num_cols = 3 + (max_seq_len > 1 ? max_seq_len - 1 : 0);
  StringGrid *sg = string_grid_create(display_pvs + 1, num_cols, 1);
  string_grid_set_cell(sg, 0, 0, string_duplicate("Play"));
  string_grid_set_cell(sg, 0, 1, string_duplicate("V"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("S"));
  for (int turn_idx = 2; turn_idx <= max_seq_len; turn_idx++) {
    string_grid_set_cell(sg, 0, turn_idx + 1,
                         get_formatted_string("Turn %d", turn_idx));
  }

  StringBuilder *tmp_sb = string_builder_create();
  for (int pv_idx = 0; pv_idx < display_pvs; pv_idx++) {
    const PVLine *pv =
        (num_pvs > 0)
            ? endgame_results_get_multi_pvline(endgame_results, pv_idx)
            : endgame_results_get_pvline(endgame_results,
                                        ENDGAME_RESULT_DISPLAY);
    const int pv_score =
        (num_pvs > 0)
            ? pv->score
            : endgame_results_get_value(endgame_results,
                                        ENDGAME_RESULT_DISPLAY);

    Game *gc = game_duplicate(source_game);
    for (int move_idx = 0; move_idx < pv->num_moves; move_idx++) {
      Move move;
      small_move_to_move(&move, &pv->moves[move_idx], game_get_board(gc));

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

      // move_idx 0 → col 0 (Play); move_idx k ≥ 1 → col k+2 (Turn k+1).
      const int col = (move_idx == 0) ? 0 : (move_idx + 2);
      string_grid_set_cell(sg, pv_idx + 1, col,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
    }
    game_destroy(gc);

    string_grid_set_cell(sg, pv_idx + 1, 1,
                         get_formatted_string("%d", pv_score));

    const int on_turn_spread =
        (on_turn == 0 ? p1_score - p2_score : p2_score - p1_score) + pv_score;
    string_grid_set_cell(sg, pv_idx + 1, 2,
                         get_formatted_string("%d", on_turn_spread));
  }
  string_builder_destroy(tmp_sb);
  string_builder_add_string_grid(pv_description, sg, false);
  string_grid_destroy(sg);
}

char *endgame_results_get_string(EndgameResults *endgame_results,
                                 const Game *game) {
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
  string_builder_endgame_results(pv_description, endgame_results, game);
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_DISPLAY);
  char *pvline_string = string_builder_dump(pv_description, NULL);
  string_builder_destroy(pv_description);
  return pvline_string;
}
