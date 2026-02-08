#include "../ent/endgame_results.h"
#include "../ent/game_history.h"
#include "../impl/gameplay.h"
#include "../str/move_string.h"
#include "../util/string_util.h"

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
  const PVLine *pv_line =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_DISPLAY);
  Move move;
  string_builder_add_formatted_string(
      pv_description,
      "Principal Variation (depth: %d, value: %d, length: %d, time: %.3fs)%c",
      depth, endgame_results_get_value(endgame_results, ENDGAME_RESULT_DISPLAY),
      pv_line->num_moves,
      endgame_results_get_display_seconds_elapsed(endgame_results),
      add_line_breaks ? '\n' : ' ');

  Game *gc = game_duplicate(game);
  const Board *board = game_get_board(gc);
  const LetterDistribution *ld = game_get_ld(gc);

  if (add_line_breaks) {
    StringGrid *sg = string_grid_create(pv_line->num_moves, 3, 1);
    StringBuilder *tmp_sb = string_builder_create();
    for (int i = 0; i < pv_line->num_moves; i++) {
      int curr_col = 0;
      // Set the player name
      const int player_on_turn = game_get_player_on_turn_index(gc);
      const char *player_name;
      if (game_history) {
        player_name =
            game_history_player_get_name(game_history, player_on_turn);
      } else {
        player_name = player_on_turn == 0 ? "Player 1" : "Player 2";
      }
      string_grid_set_cell(sg, i, curr_col++,
                           get_formatted_string("(%s)", player_name));

      // Set the play sequence index and player name
      string_grid_set_cell(sg, i, curr_col++,
                           get_formatted_string("%d:", i + 1));

      // Set the move string
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(tmp_sb, board, &move, ld, true);
      string_grid_set_cell(sg, i, curr_col++,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
      // Play the move on the board to make the next small_move_to_move make
      // sense.
      play_move(&move, gc, NULL);
    }
    string_builder_destroy(tmp_sb);
    string_builder_add_string_grid(pv_description, sg, false);
    string_grid_destroy(sg);
  } else {
    for (int i = 0; i < pv_line->num_moves; i++) {
      string_builder_add_formatted_string(pv_description, "%d: ", i + 1);
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(pv_description, board, &move, ld, true);
      if (i != pv_line->num_moves - 1) {
        string_builder_add_string(pv_description, " -> ");
      }
      // Play the move on the board to make the next small_move_to_move make
      // sense.
      play_move(&move, gc, NULL);
    }
  }
  string_builder_add_string(pv_description, "\n");
  game_destroy(gc);
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