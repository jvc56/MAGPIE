#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/move_undo.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/validated_move.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct PlayEventsData {
  Rack previously_played_letters_racks[2];
  Rack known_letters_from_phonies_racks[2];
  bool played_end_rack_penalty[2];
  bool played_time_penalty[2];
  bool played_end_rack_points;
} PlayEventsData;

Equity get_leave_value_for_move(const KLV *klv, const Move *move, Rack *rack) {
  for (int i = 0; i < move_get_tiles_length(move); i++) {
    if (move_get_tile(move, i) != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(move_get_tile(move, i))) {
        rack_take_letter(rack, BLANK_MACHINE_LETTER);
      } else {
        rack_take_letter(rack, move_get_tile(move, i));
      }
    }
  }
  return klv_get_leave_value(klv, rack);
}

// Assumes the move hasn't been played yet and is in the rack
void get_leave_for_move(const Move *move, const Game *game, Rack *leave) {
  rack_copy(leave, player_get_rack(game_get_player(
                       game, game_get_player_on_turn_index(game))));
  int tiles_length = move_get_tiles_length(move);
  for (int idx = 0; idx < tiles_length; idx++) {
    MachineLetter letter = move_get_tile(move, idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (get_is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    rack_take_letter(leave, letter);
  }
}

void play_move_on_board(const Move *move, const Game *game) {
  // PlaceMoveTiles
  Board *board = game_get_board(game);
  int row_start = move_get_row_start(move);
  int col_start = move_get_col_start(move);
  int move_dir = move_get_dir(move);

  bool board_was_transposed = false;
  if (!board_matches_dir(board, move_dir)) {
    board_transpose(board);
    board_was_transposed = true;
    row_start = move_get_col_start(move);
    col_start = move_get_row_start(move);
  }

  int tiles_length = move_get_tiles_length(move);

  for (int idx = 0; idx < tiles_length; idx++) {
    MachineLetter letter = move_get_tile(move, idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    board_set_letter(board, row_start, col_start + idx, letter);
    if (get_is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    rack_take_letter(player_get_rack(game_get_player(
                         game, game_get_player_on_turn_index(game))),
                     letter);
  }

  board_increment_tiles_played(board, move_get_tiles_played(move));

  for (int col = col_start; col < tiles_length + col_start; col++) {
    board_update_anchors(board, row_start, col);
    if (row_start > 0) {
      board_update_anchors(board, row_start - 1, col);
    }
    if (row_start < BOARD_DIM - 1) {
      board_update_anchors(board, row_start + 1, col);
    }
  }
  if (col_start - 1 >= 0) {
    board_update_anchors(board, row_start, col_start - 1);
  }
  if (tiles_length + col_start < BOARD_DIM) {
    board_update_anchors(board, row_start, tiles_length + col_start);
  }

  if (board_was_transposed) {
    board_transpose(board);
  }
}

void calc_for_across(const Move *move, const Game *game, int row_start,
                     int col_start, int csd) {
  for (int row = row_start; row < move_get_tiles_length(move) + row_start;
       row++) {
    if (move_get_tile(move, row - row_start) == PLAYED_THROUGH_MARKER) {
      continue;
    }

    const Board *board = game_get_board(game);
    const bool kwgs_are_shared =
        game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    const int right_col =
        board_get_word_edge(board, row, col_start, WORD_DIRECTION_RIGHT);
    const int left_col =
        board_get_word_edge(board, row, col_start, WORD_DIRECTION_LEFT);
    game_gen_cross_set(game, row, right_col + 1, csd, 0);
    game_gen_cross_set(game, row, left_col - 1, csd, 0);
    game_gen_cross_set(game, row, col_start, csd, 0);
    if (!kwgs_are_shared) {
      game_gen_cross_set(game, row, right_col + 1, csd, 1);
      game_gen_cross_set(game, row, left_col - 1, csd, 1);
      game_gen_cross_set(game, row, col_start, csd, 1);
    }
  }
}

void calc_for_self(const Move *move, const Game *game, int row_start,
                   int col_start, int csd) {
  for (int col = col_start - 1; col <= col_start + move_get_tiles_length(move);
       col++) {
    game_gen_cross_set(game, row_start, col, csd, 0);
  }
  if (!game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG)) {
    for (int col = col_start - 1;
         col <= col_start + move_get_tiles_length(move); col++) {
      game_gen_cross_set(game, row_start, col, csd, 1);
    }
  }
}

// Update cross-sets for move region after unplay. Unlike calc_for_across,
// this doesn't use board_get_word_edge (which requires tiles to be on board).
// Instead, it directly iterates over the move's tile positions.
static void calc_for_across_after_unplay(const Move *move, const Game *game,
                                         int row_start, int col_start,
                                         int csd) {
  const Board *board = game_get_board(game);
  const bool kwgs_are_shared =
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);

  for (int row = row_start; row < move_get_tiles_length(move) + row_start;
       row++) {
    if (move_get_tile(move, row - row_start) == PLAYED_THROUGH_MARKER) {
      continue;
    }

    // For each tile position, find the word edges by looking at adjacent tiles
    // that are still on the board (pre-existing tiles, not the ones we just
    // unplayed)
    int left_col = col_start - 1;
    while (left_col >= 0 && !board_is_empty_or_bricked(board, row, left_col)) {
      left_col--;
    }
    int right_col = col_start + 1;
    while (right_col < BOARD_DIM &&
           !board_is_empty_or_bricked(board, row, right_col)) {
      right_col++;
    }

    // Update cross-sets at left edge, right edge, and the move position itself
    game_gen_cross_set(game, row, left_col, csd, 0);
    game_gen_cross_set(game, row, right_col, csd, 0);
    game_gen_cross_set(game, row, col_start, csd, 0);
    if (!kwgs_are_shared) {
      game_gen_cross_set(game, row, left_col, csd, 1);
      game_gen_cross_set(game, row, right_col, csd, 1);
      game_gen_cross_set(game, row, col_start, csd, 1);
    }
  }
}

// Update cross-sets for squares affected by a move after unplaying it.
// This version doesn't rely on board_get_word_edge (which assumes tiles are on
// board).
void update_cross_sets_after_unplay(const Move *move, const Game *game) {
  Board *board = game_get_board(game);
  if (board_is_dir_vertical(move_get_dir(move))) {
    calc_for_across_after_unplay(move, game, move_get_row_start(move),
                                 move_get_col_start(move),
                                 BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_self(move, game, move_get_col_start(move),
                  move_get_row_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  } else {
    calc_for_self(move, game, move_get_row_start(move),
                  move_get_col_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_across_after_unplay(move, game, move_get_col_start(move),
                                 move_get_row_start(move),
                                 BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  }
}

// --- MoveUndo-based cross-set update functions ---
// These use the tiles_placed_mask instead of the Move's tiles array

// calc_for_across using MoveUndo (for forward update when tiles are on board)
static void calc_for_across_from_undo(const MoveUndo *undo, const Game *game,
                                      int row_start, int col_start, int csd) {
  const Board *board = game_get_board(game);
  const bool kwgs_are_shared =
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);

  for (int i = 0; i < undo->move_tiles_length; i++) {
    // Check tiles_placed_mask: bit i is set if position i had an actual tile
    if ((undo->tiles_placed_mask & (1 << i)) == 0) {
      continue; // This was a played-through position, skip it
    }

    int row = row_start + i;
    const int right_col =
        board_get_word_edge(board, row, col_start, WORD_DIRECTION_RIGHT);
    const int left_col =
        board_get_word_edge(board, row, col_start, WORD_DIRECTION_LEFT);
    game_gen_cross_set(game, row, right_col + 1, csd, 0);
    game_gen_cross_set(game, row, left_col - 1, csd, 0);
    game_gen_cross_set(game, row, col_start, csd, 0);
    if (!kwgs_are_shared) {
      game_gen_cross_set(game, row, right_col + 1, csd, 1);
      game_gen_cross_set(game, row, left_col - 1, csd, 1);
      game_gen_cross_set(game, row, col_start, csd, 1);
    }
  }
}

// calc_for_self using MoveUndo (doesn't need tiles info, just length)
static void calc_for_self_from_undo(const MoveUndo *undo, const Game *game,
                                    int row_start, int col_start, int csd) {
  for (int col = col_start - 1; col <= col_start + undo->move_tiles_length;
       col++) {
    game_gen_cross_set(game, row_start, col, csd, 0);
  }
  if (!game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG)) {
    for (int col = col_start - 1; col <= col_start + undo->move_tiles_length;
         col++) {
      game_gen_cross_set(game, row_start, col, csd, 1);
    }
  }
}

// calc_for_across after unplay using MoveUndo
static void calc_for_across_after_unplay_from_undo(const MoveUndo *undo,
                                                   const Game *game,
                                                   int row_start, int col_start,
                                                   int csd) {
  const Board *board = game_get_board(game);
  const bool kwgs_are_shared =
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);

  for (int i = 0; i < undo->move_tiles_length; i++) {
    // Check tiles_placed_mask: bit i is set if position i had an actual tile
    if ((undo->tiles_placed_mask & (1 << i)) == 0) {
      continue; // This was a played-through position, skip it
    }

    int row = row_start + i;

    // For each tile position, find the word edges by looking at adjacent tiles
    // that are still on the board (pre-existing tiles, not the ones we just
    // unplayed)
    int left_col = col_start - 1;
    while (left_col >= 0 && !board_is_empty_or_bricked(board, row, left_col)) {
      left_col--;
    }
    int right_col = col_start + 1;
    while (right_col < BOARD_DIM &&
           !board_is_empty_or_bricked(board, row, right_col)) {
      right_col++;
    }

    // Update cross-sets at left edge, right edge, and the move position itself
    game_gen_cross_set(game, row, left_col, csd, 0);
    game_gen_cross_set(game, row, right_col, csd, 0);
    game_gen_cross_set(game, row, col_start, csd, 0);
    if (!kwgs_are_shared) {
      game_gen_cross_set(game, row, left_col, csd, 1);
      game_gen_cross_set(game, row, right_col, csd, 1);
      game_gen_cross_set(game, row, col_start, csd, 1);
    }
  }
}

// Update cross-sets for move region using MoveUndo (forward update)
void update_cross_set_for_move_from_undo(const MoveUndo *undo,
                                         const Game *game) {
  Board *board = game_get_board(game);
  if (board_is_dir_vertical(undo->move_dir)) {
    calc_for_across_from_undo(undo, game, undo->move_row_start,
                              undo->move_col_start, BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_self_from_undo(undo, game, undo->move_col_start,
                            undo->move_row_start, BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  } else {
    calc_for_self_from_undo(undo, game, undo->move_row_start,
                            undo->move_col_start, BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_across_from_undo(undo, game, undo->move_col_start,
                              undo->move_row_start, BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  }
}

// Update cross-sets after unplaying using MoveUndo
void update_cross_sets_after_unplay_from_undo(const MoveUndo *undo,
                                              const Game *game) {
  Board *board = game_get_board(game);
  if (board_is_dir_vertical(undo->move_dir)) {
    calc_for_across_after_unplay_from_undo(undo, game, undo->move_row_start,
                                           undo->move_col_start,
                                           BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_self_from_undo(undo, game, undo->move_col_start,
                            undo->move_row_start, BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  } else {
    calc_for_self_from_undo(undo, game, undo->move_row_start,
                            undo->move_col_start, BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_across_after_unplay_from_undo(undo, game, undo->move_col_start,
                                           undo->move_row_start,
                                           BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  }
}

void update_cross_set_for_move(const Move *move, const Game *game) {
  Board *board = game_get_board(game);
  if (board_is_dir_vertical(move_get_dir(move))) {
    calc_for_across(move, game, move_get_row_start(move),
                    move_get_col_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_self(move, game, move_get_col_start(move),
                  move_get_row_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  } else {
    calc_for_self(move, game, move_get_row_start(move),
                  move_get_col_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
    calc_for_across(move, game, move_get_col_start(move),
                    move_get_row_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  }
}

// Draws the required number of tiles to fill the rack to RACK_SIZE.
void draw_to_full_rack(const Game *game, const int player_index) {
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  const int player_draw_index = game_get_player_draw_index(game, player_index);
  int num_to_draw = RACK_SIZE - rack_get_total_letters(player_rack);
  while (num_to_draw > 0 && !bag_is_empty(bag)) {
    rack_add_letter(player_rack,
                    bag_draw_random_letter(bag, player_draw_index));
    num_to_draw--;
  }
}

// Returns true if there are enough tiles in bag and player_rack
// to draw rack_to_draw.
bool rack_is_drawable(const Game *game, const int player_index,
                      const Rack *rack_to_draw) {
  const Bag *bag = game_get_bag(game);
  const Rack *player_rack =
      player_get_rack(game_get_player(game, player_index));
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    if (bag_get_letter(bag, i) + rack_get_letter(player_rack, i) <
        rack_get_letter(rack_to_draw, i)) {
      return false;
    }
  }
  return true;
}

// Draws a nonrandom set of letters specified by rack_to_draw from the
// bag to the rack. Assumes the rack is empty.
// Returns true on success.
// Return false when the rack letters are not in the bag.
bool draw_rack_from_bag(const Game *game, const int player_index,
                        const Rack *rack_to_draw) {
  if (rack_get_dist_size(rack_to_draw) == 0) {
    // Rack is effectively NULL
    return true;
  }
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  int player_draw_index = game_get_player_draw_index(game, player_index);
  rack_copy(player_rack, rack_to_draw);
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    const uint16_t rack_number_of_letter = rack_get_letter(player_rack, i);
    for (uint16_t j = 0; j < rack_number_of_letter; j++) {
      if (!bag_draw_letter(bag, i, player_draw_index)) {
        return false;
      }
    }
  }
  return true;
}

// Draws whatever the tiles in rack_to_draw from the bag to rack_to_update.
// If there are not enough tiles in the bag, it continues to draw normally
// and just returns whatever tiles were available.
void draw_leave_from_bag(Bag *bag, int player_draw_index, Rack *rack_to_update,
                         const Rack *rack_to_draw) {
  const uint16_t dist_size = rack_get_dist_size(rack_to_draw);
  for (int i = 0; i < dist_size; i++) {
    const uint16_t rack_number_of_letter = rack_get_letter(rack_to_draw, i);
    for (uint16_t j = 0; j < rack_number_of_letter; j++) {
      if (!bag_draw_letter(bag, i, player_draw_index)) {
        continue;
      }
      rack_add_letter(rack_to_update, i);
    }
  }
}

// Draws a nonrandom set of letters specified by rack_string from the
// bag to the rack. Assumes the rack is empty.
// Returns number of letters drawn on success
// Returns -1 if the string was malformed.
// Returns -2 if the tiles were not in the bag.
int draw_rack_string_from_bag(const Game *game, const int player_index,
                              const char *rack_string) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack player_rack_copy;
  rack_set_dist_size_and_reset(&player_rack_copy, ld_get_size(ld));
  int number_of_letters_set =
      rack_set_to_string(ld, &player_rack_copy, rack_string);

  if (number_of_letters_set != -1) {
    if (!rack_is_drawable(game, player_index, &player_rack_copy)) {
      number_of_letters_set = -2;
    } else {
      draw_rack_from_bag(game, player_index, &player_rack_copy);
    }
  }

  return number_of_letters_set;
}

void return_rack_to_bag(const Game *game, const int player_index) {
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  int player_draw_index = game_get_player_draw_index(game, player_index);
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    const uint16_t rack_number_of_letter = rack_get_letter(player_rack, i);
    for (uint16_t j = 0; j < rack_number_of_letter; j++) {
      bag_add_letter(bag, i, player_draw_index);
    }
  }
  rack_reset(player_rack);
}

void set_random_rack(const Game *game, const int player_index,
                     const Rack *known_rack) {
  return_rack_to_bag(game, player_index);
  if (known_rack) {
    if (!draw_rack_from_bag(game, player_index, known_rack)) {
      const LetterDistribution *ld = game_get_ld(game);
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "unexpectedly failed to draw rack '");
      string_builder_add_rack(sb, known_rack, ld, false);
      string_builder_add_string(sb, "' from the bag");
      char *err_msg = string_builder_dump(sb, NULL);
      string_builder_destroy(sb);
      log_fatal(err_msg);
      // Unreachable, but will silence static analyzer warnings
      free(err_msg);
    }
  }
  draw_to_full_rack(game, player_index);
}

void execute_exchange_move(const Move *move, const Game *game, Rack *leave) {
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Rack *player_on_turn_rack =
      player_get_rack(game_get_player(game, player_on_turn_index));
  Bag *bag = game_get_bag(game);

  const int num_tiles_exchanged = move_get_tiles_played(move);

  for (int i = 0; i < num_tiles_exchanged; i++) {
    rack_take_letter(player_on_turn_rack, move_get_tile(move, i));
  }

  if (leave) {
    rack_copy(leave, player_on_turn_rack);
  }

  draw_to_full_rack(game, player_on_turn_index);
  assert(rack_get_total_letters(player_on_turn_rack) == RACK_SIZE);
  int player_draw_index = game_get_player_on_turn_draw_index(game);
  for (int i = 0; i < num_tiles_exchanged; i++) {
    bag_add_letter(bag, move_get_tile(move, i), player_draw_index);
  }
}

Equity calculate_end_rack_points(const Rack *rack,
                                 const LetterDistribution *ld) {
  // Note: updating this function will require updates to the endgame adjustment
  // functions in static_eval.h
  return 2 * rack_get_score(ld, rack);
}

Equity calculate_end_rack_penalty(const Rack *rack,
                                  const LetterDistribution *ld) {
  // Note: updating this function will require updates to the endgame adjustment
  // functions in static_eval.h
  return -rack_get_score(ld, rack);
}

void standard_end_of_game_calculations(Game *game) {
  int player_on_turn_index = game_get_player_on_turn_index(game);

  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  const Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  const LetterDistribution *ld = game_get_ld(game);

  player_add_to_score(player_on_turn,
                      calculate_end_rack_points(player_get_rack(opponent), ld));
  game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
}

void draw_starting_racks(const Game *game) {
  draw_to_full_rack(game, 0);
  draw_to_full_rack(game, 1);
}

// Assumes the move has been validated
// If the input leave rack is not null, it will record the leave of
// the play in the leave rack.
void play_move(const Move *move, Game *game, Rack *leave) {
  game_backup(game);
  const LetterDistribution *ld = game_get_ld(game);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  const Rack *player_on_turn_rack = player_get_rack(player_on_turn);
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, game);
    if (leave) {
      rack_copy(leave, player_on_turn_rack);
    }
    update_cross_set_for_move(move, game);
    game_set_consecutive_scoreless_turns(game, 0);

    player_add_to_score(player_on_turn, move_get_score(move));
    draw_to_full_rack(game, player_on_turn_index);
    if (rack_is_empty(player_on_turn_rack)) {
      standard_end_of_game_calculations(game);
    }
  } else if (move_get_type(move) == GAME_EVENT_PASS) {
    if (leave) {
      rack_copy(leave, player_on_turn_rack);
    }
    game_increment_consecutive_scoreless_turns(game);
  } else if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    execute_exchange_move(move, game, leave);
    game_increment_consecutive_scoreless_turns(game);
  }
  if (game_reached_max_scoreless_turns(game)) {
    Player *player0 = game_get_player(game, 0);
    Player *player1 = game_get_player(game, 1);
    player_add_to_score(
        player0, calculate_end_rack_penalty(player_get_rack(player0), ld));
    player_add_to_score(
        player1, calculate_end_rack_penalty(player_get_rack(player1), ld));
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  }
  game_start_next_player_turn(game);
}

void play_move_without_drawing_tiles(const Move *move, Game *game) {
  game_backup(game);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  const Rack *player_on_turn_rack = player_get_rack(player_on_turn);
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, game);
    update_cross_set_for_move(move, game);
    game_set_consecutive_scoreless_turns(game, 0);
    player_add_to_score(player_on_turn, move_get_score(move));
    if (rack_is_empty(player_on_turn_rack) &&
        bag_get_letters(game_get_bag(game)) <= (RACK_SIZE)) {
      game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
    }
  } else if (move_get_type(move) == GAME_EVENT_PASS) {
    game_increment_consecutive_scoreless_turns(game);
  } else if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    execute_exchange_move(move, game, NULL);
    game_increment_consecutive_scoreless_turns(game);
  }
  if (game_reached_max_scoreless_turns(game)) {
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  }
  game_start_next_player_turn(game);
}

void return_phony_letters(Game *game) {
  game_unplay_last_move(game);
  game_start_next_player_turn(game);
  game_increment_consecutive_scoreless_turns(game);
}

static void play_move_on_board_tracked(const Move *move, const Game *game,
                                       MoveUndo *undo) {
  Board *board = game_get_board(game);
  int row_start = move_get_row_start(move);
  int col_start = move_get_col_start(move);
  int move_dir = move_get_dir(move);

  bool board_was_transposed = false;
  if (!board_matches_dir(board, move_dir)) {
    board_transpose(board);
    board_was_transposed = true;
    row_start = move_get_col_start(move);
    col_start = move_get_row_start(move);
  }

  int tiles_length = move_get_tiles_length(move);

  for (int idx = 0; idx < tiles_length; idx++) {
    MachineLetter letter = move_get_tile(move, idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    board_set_letter_tracked(board, row_start, col_start + idx, letter, undo);
    if (get_is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    rack_take_letter(player_get_rack(game_get_player(
                         game, game_get_player_on_turn_index(game))),
                     letter);
  }

  // Update tiles_played (old value already saved by play_move_incremental)
  board_increment_tiles_played(board, move_get_tiles_played(move));

  // Update anchors (old number_of_row_anchors already saved by
  // play_move_incremental)
  for (int col = col_start; col < tiles_length + col_start; col++) {
    board_update_anchors_tracked(board, row_start, col, undo);
    if (row_start > 0) {
      board_update_anchors_tracked(board, row_start - 1, col, undo);
    }
    if (row_start < BOARD_DIM - 1) {
      board_update_anchors_tracked(board, row_start + 1, col, undo);
    }
  }
  if (col_start - 1 >= 0) {
    board_update_anchors_tracked(board, row_start, col_start - 1, undo);
  }
  if (tiles_length + col_start < BOARD_DIM) {
    board_update_anchors_tracked(board, row_start, tiles_length + col_start,
                                 undo);
  }

  if (board_was_transposed) {
    board_transpose(board);
  }
}

void play_move_incremental(const Move *move, Game *game, MoveUndo *undo) {
  move_undo_reset(undo);

  // Save game state
  undo->player_on_turn_index = game_get_player_on_turn_index(game);
  undo->old_consecutive_scoreless_turns =
      game_get_consecutive_scoreless_turns(game);
  undo->old_game_end_reason = game_get_game_end_reason(game);

  Player *player_on_turn = game_get_player(game, undo->player_on_turn_index);
  const Rack *player_on_turn_rack = player_get_rack(player_on_turn);

  // Save rack and both players' scores
  rack_copy(&undo->old_rack, player_on_turn_rack);
  undo->old_scores[0] = player_get_score(game_get_player(game, 0));
  undo->old_scores[1] = player_get_score(game_get_player(game, 1));

  // Save board state
  Board *board = game_get_board(game);
  undo->old_tiles_played = board_get_tiles_played(board);
  undo->old_cross_sets_valid = board_get_cross_sets_valid(board);
  memcpy(undo->old_number_of_row_anchors, board->number_of_row_anchors,
         sizeof(undo->old_number_of_row_anchors));

  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    // Save move info for lazy cross-set updates
    undo->move_row_start = (uint8_t)move_get_row_start(move);
    undo->move_col_start = (uint8_t)move_get_col_start(move);
    undo->move_tiles_length = (uint8_t)move_get_tiles_length(move);
    undo->move_dir = (uint8_t)move_get_dir(move);

    // Build tiles_placed_mask: bit i set if position i had an actual tile
    // placed
    uint16_t tiles_placed_mask = 0;
    for (int i = 0; i < move_get_tiles_length(move); i++) {
      if (move_get_tile(move, i) != PLAYED_THROUGH_MARKER) {
        tiles_placed_mask |= (uint16_t)(1 << i);
      }
    }
    undo->tiles_placed_mask = tiles_placed_mask;

    play_move_on_board_tracked(move, game, undo);
    // Cross-sets are NOT updated here - they are computed lazily before move
    // generation. The caller can call update_cross_set_for_move if eager update
    // is needed, or leave them invalid for lazy evaluation.
    board_set_cross_sets_valid(board, false);
    game_set_consecutive_scoreless_turns(game, 0);

    player_add_to_score(player_on_turn, move_get_score(move));

    // Note: No tile drawing needed - incremental play/unplay is only used
    // during endgame when the bag is empty.

    if (rack_is_empty(player_on_turn_rack)) {
      const LetterDistribution *ld = game_get_ld(game);
      const Player *opponent =
          game_get_player(game, 1 - undo->player_on_turn_index);
      // Standard end game: player who went out gets 2x opponent's rack value
      Equity end_rack_points =
          calculate_end_rack_points(player_get_rack(opponent), ld);
      player_add_to_score(player_on_turn, end_rack_points);
      game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
    }
  } else if (move_get_type(move) == GAME_EVENT_PASS) {
    game_increment_consecutive_scoreless_turns(game);
  }
  // Note: EXCHANGE is not typically used in endgame

  if (game_reached_max_scoreless_turns(game)) {
    const LetterDistribution *ld = game_get_ld(game);
    Player *player0 = game_get_player(game, 0);
    Player *player1 = game_get_player(game, 1);
    player_add_to_score(
        player0, calculate_end_rack_penalty(player_get_rack(player0), ld));
    player_add_to_score(
        player1, calculate_end_rack_penalty(player_get_rack(player1), ld));
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  }
  game_start_next_player_turn(game);
}

void unplay_move_incremental(Game *game, const MoveUndo *undo) {
  // Restore player turn
  game_set_player_on_turn_index(game, undo->player_on_turn_index);

  // Restore game state
  game_set_consecutive_scoreless_turns(game,
                                       undo->old_consecutive_scoreless_turns);
  game_set_game_end_reason(game, undo->old_game_end_reason);

  // Restore player rack and both players' scores
  Player *player_on_turn =
      game_get_player(game, undo->player_on_turn_index);
  rack_copy(player_get_rack(player_on_turn), &undo->old_rack);
  player_set_score(game_get_player(game, 0), undo->old_scores[0]);
  player_set_score(game_get_player(game, 1), undo->old_scores[1]);

  // Note: No bag state restoration needed - incremental play/unplay is only
  // used during endgame when the bag is empty.

  // Restore board
  Board *board = game_get_board(game);
  move_undo_restore_squares(undo, board);
  board_set_tiles_played(board, undo->old_tiles_played);
  board_set_cross_sets_valid(board, undo->old_cross_sets_valid);
  memcpy(board->number_of_row_anchors, undo->old_number_of_row_anchors,
         sizeof(board->number_of_row_anchors));
}

// Optimized play for endgame outplays (player plays all tiles).
// Skips board update and cross-set generation since game ends immediately.
// Only updates scores and game state - much faster for endgame search.
void play_move_endgame_outplay(const Move *move, Game *game, MoveUndo *undo) {
  move_undo_reset(undo);

  // Save game state for undo
  undo->player_on_turn_index = game_get_player_on_turn_index(game);
  undo->old_consecutive_scoreless_turns =
      game_get_consecutive_scoreless_turns(game);
  undo->old_game_end_reason = game_get_game_end_reason(game);

  Player *player_on_turn = game_get_player(game, undo->player_on_turn_index);
  const Player *opponent =
      game_get_player(game, 1 - undo->player_on_turn_index);
  const LetterDistribution *ld = game_get_ld(game);

  // Save rack (even though we don't modify it, unplay expects it)
  rack_copy(&undo->old_rack, player_get_rack(player_on_turn));

  // Save scores for undo
  undo->old_scores[0] = player_get_score(game_get_player(game, 0));
  undo->old_scores[1] = player_get_score(game_get_player(game, 1));

  // Save board state (even though we don't modify it, unplay expects it)
  const Board *board = game_get_board(game);
  undo->old_tiles_played = board_get_tiles_played(board);
  undo->old_cross_sets_valid = board_get_cross_sets_valid(board);
  memcpy(undo->old_number_of_row_anchors, board->number_of_row_anchors,
         sizeof(undo->old_number_of_row_anchors));

  // Add move score to player
  player_add_to_score(player_on_turn, move_get_score(move));

  // Add end rack bonus (2x opponent's rack value)
  player_add_to_score(player_on_turn,
                      calculate_end_rack_points(player_get_rack(opponent), ld));

  // Mark game as ended
  game_set_game_end_reason(game, GAME_END_REASON_STANDARD);

  // Switch turns for negamax sign flip
  game_start_next_player_turn(game);
}

void generate_moves_for_game_override_record_type(
    const MoveGenArgs *args, move_record_t move_record_type) {
  const Player *player_on_turn =
      game_get_player(args->game, game_get_player_on_turn_index(args->game));

  const MoveGenArgs args_with_overwritten_record_and_sort = {
      .game = args->game,
      .move_list = args->move_list,
      .move_record_type = move_record_type,
      .move_sort_type = player_get_move_sort_type(player_on_turn),
      .override_kwg = NULL,
      .thread_index = args->thread_index,
      .eq_margin_movegen = args->eq_margin_movegen,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  generate_moves(&args_with_overwritten_record_and_sort);
}

// Overwrites the passed in values for the following MoveGenArgs fields:
// - move_record_type (with the player on turn's record type)
// - move_sort_type (with the player on turn's sort type)
// - override_kwg (with NULL)
void generate_moves_for_game(const MoveGenArgs *args) {
  generate_moves_for_game_override_record_type(
      args, player_get_move_record_type(game_get_player(
                args->game, game_get_player_on_turn_index(args->game))));
}

Move *get_top_equity_move(Game *game, int thread_index, MoveList *move_list) {
  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_BEST,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .thread_index = thread_index,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .target_leave_size_for_exchange_cutoff =
                                UNSET_LEAVE_SIZE};
  generate_moves(&args);
  return move_list_get_move(move_list, 0);
}

Move *get_top_equity_move_for_inferences(
    Game *game, int thread_index, MoveList *move_list, Equity target_equity,
    int target_leave_size_for_exchange_cutoff, Equity equity_margin) {
  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_BEST,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .thread_index = thread_index,
                            .eq_margin_movegen = equity_margin,
                            .target_equity = target_equity,
                            .target_leave_size_for_exchange_cutoff =
                                target_leave_size_for_exchange_cutoff};
  generate_moves(&args);
  return move_list_get_move(move_list, 0);
}

void validate_challenge_bonus_order(const GameEvent *game_event,
                                    const GameEvent *previous_game_event,
                                    ErrorStack *error_stack) {
  const int game_event_player_index = game_event_get_player_index(game_event);
  if (game_event_get_type(previous_game_event) !=
      GAME_EVENT_TILE_PLACEMENT_MOVE) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_CHALLENGE_BONUS_WITHOUT_PLAY,
        string_duplicate("encountered a challenge bonus without a play"));
    return;
  }
  if (game_event_get_player_index(previous_game_event) !=
      game_event_player_index) {
    error_stack_push(
        error_stack,
        ERROR_STATUS_GCG_PARSE_INVALID_CHALLENGE_BONUS_PLAYER_INDEX,
        string_duplicate("encountered a challenge bonus for the wrong player"));
    return;
  }
}

void validate_phony_tiles_returned_order(const GameEvent *game_event,
                                         const GameEvent *previous_game_event,
                                         ErrorStack *error_stack) {
  const int game_event_player_index = game_event_get_player_index(game_event);
  if (game_event_get_type(previous_game_event) !=
      GAME_EVENT_TILE_PLACEMENT_MOVE) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_WITHOUT_PLAY,
        string_duplicate("encountered a phony letters return event without "
                         "a previous tile placement move"));
    return;
  }
  if (game_event_get_player_index(previous_game_event) !=
      game_event_player_index) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_INVALID_PHONY_TILES_PLAYER_INDEX,
        string_duplicate(
            "encountered a phony letters return event for the wrong player"));
    return;
  }
  if (!racks_are_equal(game_event_get_const_rack(game_event),
                       game_event_get_const_rack(previous_game_event))) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_MISMATCH,
        string_duplicate(
            "phony letters played do not match the phony letters returned"));
    return;
  }
}

void validate_challenge_bonus_and_phony_tiles_returned_order(
    const GameEvent *game_event, const GameEvent *previous_game_event,
    ErrorStack *error_stack) {
  switch (game_event_get_type(game_event)) {
  case GAME_EVENT_CHALLENGE_BONUS:
    validate_challenge_bonus_order(game_event, previous_game_event,
                                   error_stack);
    break;
  case GAME_EVENT_PHONY_TILES_RETURNED:
    validate_phony_tiles_returned_order(game_event, previous_game_event,
                                        error_stack);
    break;
  default:
    return;
  }
}

void validate_time_penalty_order(const GameEvent *game_event,
                                 const GameEvent *previous_game_event,
                                 const GameEvent *next_game_event,
                                 ErrorStack *error_stack) {

  if (game_event_get_type(game_event) != GAME_EVENT_TIME_PENALTY) {
    return;
  }

  if (!previous_game_event) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY,
        string_duplicate(
            "encountered a time penalty event before the end rack event(s)"));
    return;
  }

  const game_event_t prev_ge_type = game_event_get_type(previous_game_event);
  if (prev_ge_type != GAME_EVENT_END_RACK_POINTS &&
      prev_ge_type != GAME_EVENT_END_RACK_PENALTY &&
      prev_ge_type != GAME_EVENT_TIME_PENALTY) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY,
        string_duplicate(
            "encountered a time penalty event before the end rack event(s)"));
    return;
  }

  if (!next_game_event) {
    return;
  }

  const game_event_t next_ge_type = game_event_get_type(next_game_event);
  if (next_ge_type != GAME_EVENT_TIME_PENALTY) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY,
        string_duplicate(
            "encountered a time penalty event before the end rack event(s)"));
    return;
  }
}

// Validates that the game event player indexes are
// aligned with the game and enforces game event sequence
// logic
void validate_game_event_order_and_index(const GameEvent *game_event,
                                         const GameEvent *previous_game_event,
                                         const GameEvent *next_game_event,
                                         int game_player_on_turn_index,
                                         bool game_is_over,
                                         ErrorStack *error_stack) {
  const game_event_t game_event_type = game_event_get_type(game_event);
  const int game_event_player_index = game_event_get_player_index(game_event);
  switch (game_event_type) {
  case GAME_EVENT_TILE_PLACEMENT_MOVE:
  case GAME_EVENT_EXCHANGE:
  case GAME_EVENT_PASS:
    // If this is an actual turn as opposed to a time or points penalty,
    // the Game object player on turn and the GCG player on turn should
    // match, since the play_move function would have updated the
    // player on turn index.
    if (game_player_on_turn_index != game_event_player_index) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_GAME_EVENT_OFF_TURN,
          get_formatted_string("encountered an off turn move: %s",
                               game_event_get_cgp_move_string(game_event)));
      return;
    }
    if (game_is_over) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_EVENT_AFTER_GAME_END,
          get_formatted_string("encountered a move after the game ended: %s",
                               game_event_get_cgp_move_string(game_event)));
      return;
    }
    break;
  case GAME_EVENT_CHALLENGE_BONUS:
    validate_challenge_bonus_order(game_event, previous_game_event,
                                   error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    break;
  case GAME_EVENT_PHONY_TILES_RETURNED:
    validate_phony_tiles_returned_order(game_event, previous_game_event,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    break;
  case GAME_EVENT_END_RACK_POINTS:
  case GAME_EVENT_END_RACK_PENALTY:
  case GAME_EVENT_TIME_PENALTY:
    if (!game_is_over) {
      char *err_str;
      if (game_event_type == GAME_EVENT_END_RACK_PENALTY) {
        err_str = string_duplicate(
            "encountered an end rack penalty event before the game "
            "ended");
      } else if (game_event_type == GAME_EVENT_END_RACK_POINTS) {
        err_str = string_duplicate(
            "encountered an end rack points event before the game "
            "ended");
      } else {
        err_str = string_duplicate(
            "encountered an end time penalty event before the game "
            "ended");
      }
      error_stack_push(error_stack,
                       ERROR_STATUS_GCG_PARSE_END_GAME_EVENT_BEFORE_GAME_END,
                       err_str);
      return;
    } else {
      validate_time_penalty_order(game_event, previous_game_event,
                                  next_game_event, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
      break;
    }
    break;
  case GAME_EVENT_UNKNOWN:
    log_fatal("encountered unknown game event in order and index validation");
    break;
  }
}

void copy_bag_to_rack(const Bag *bag, const Rack *rack_to_sub, Rack *rack) {
  int remaining_letters[MAX_ALPHABET_SIZE];
  memset(remaining_letters, 0, sizeof(remaining_letters));
  bag_increment_unseen_count(bag, remaining_letters);
  const int ld_size = rack_get_dist_size(rack);
  rack_set_dist_size_and_reset(rack, ld_size);
  for (MachineLetter ml = 0; ml < ld_size; ml++) {
    int letters_to_sub = 0;
    if (rack_to_sub) {
      letters_to_sub = rack_get_letter(rack_to_sub, ml);
    }
    rack_add_letters(rack, ml, remaining_letters[ml] - letters_to_sub);
  }
}

// Assumes both player racks have been returned to the bag
void set_after_game_event_racks(const GameHistory *game_history,
                                const Game *game, int game_event_index,
                                PlayEventsData *play_events_data,
                                const bool all_letters_on_rack_played,
                                ErrorStack *error_stack) {
  GameEvent *current_game_event =
      game_history_get_event(game_history, game_event_index);
  Rack *after_event_player_on_turn_rack =
      game_event_get_after_event_player_on_turn_rack(current_game_event);
  Rack *after_event_player_off_turn_rack =
      game_event_get_after_event_player_off_turn_rack(current_game_event);
  const int ld_size = ld_get_size(game_get_ld(game));
  rack_set_dist_size_and_reset(after_event_player_on_turn_rack, ld_size);
  rack_set_dist_size_and_reset(after_event_player_off_turn_rack, ld_size);

  const int player_on_turn_index = game_get_player_on_turn_index(game);
  const int number_of_game_events = game_history_get_num_events(game_history);
  bool player_on_turn_rack_set = false;
  const Bag *bag = game_get_bag(game);
  const int num_letters_in_bag = bag_get_letters(bag);
  if (all_letters_on_rack_played && num_letters_in_bag <= (RACK_SIZE)) {
    // If the bag has <= RACK_SIZE, then the last play must've been an outplay
    // and the opp must have all of the remaining letters.
    copy_bag_to_rack(bag, NULL, after_event_player_on_turn_rack);
    player_on_turn_rack_set = true;
  } else {
    for (int i = game_event_index + 1; i < number_of_game_events; i++) {
      const GameEvent *game_event_i = game_history_get_event(game_history, i);
      if (i > 0) {
        // Since this loop is looking ahead into the game history slightly
        // it might encounter game event order errors, so we check them
        // here so the potential errors are more coherent and a side effect
        // error is not returned later.
        validate_challenge_bonus_and_phony_tiles_returned_order(
            game_event_i, game_history_get_event(game_history, i - 1),
            error_stack);
        if (!error_stack_is_empty(error_stack)) {
          return;
        }
      }
      if (game_event_get_player_index(game_event_i) == player_on_turn_index &&
          game_event_get_type(game_event_i) != GAME_EVENT_END_RACK_POINTS &&
          rack_get_dist_size(game_event_get_const_rack(game_event_i)) != 0) {
        rack_copy(after_event_player_on_turn_rack,
                  game_event_get_const_rack(game_event_i));
        player_on_turn_rack_set = true;
        break;
      }
    }
  }

  if (!player_on_turn_rack_set) {
    const Rack *player_on_turn_last_rack =
        game_history_player_get_last_rack_const(game_history,
                                                player_on_turn_index);
    if (rack_get_dist_size(player_on_turn_last_rack) != 0) {
      rack_copy(after_event_player_on_turn_rack,
                game_history_player_get_last_rack_const(game_history,
                                                        player_on_turn_index));
    }
  }

  if (num_letters_in_bag -
          rack_get_total_letters(after_event_player_on_turn_rack) <=
      (RACK_SIZE)) {
    // If there are no more than RACK_SIZE tiles in the bag, that means the
    // player off turn will necessarily have all of them and the actual game bag
    // is empty.
    copy_bag_to_rack(bag, after_event_player_on_turn_rack,
                     after_event_player_off_turn_rack);
  } else {
    // Otherwise, the rack is only known from the previous phonies the player
    // made
    rack_copy(
        after_event_player_off_turn_rack,
        &play_events_data
             ->known_letters_from_phonies_racks[1 - player_on_turn_index]);
    if (num_letters_in_bag -
            rack_get_total_letters(after_event_player_off_turn_rack) <=
        (RACK_SIZE)) {
      // The known rack from phonies is all of the remaining letters in the bag
      // so we now the current player on turn rack
      copy_bag_to_rack(bag, after_event_player_off_turn_rack,
                       after_event_player_on_turn_rack);
    }
  }
}

void set_rack_from_bag_or_push_to_error_stack(const Game *game,
                                              const int player_index,
                                              const Rack *rack_to_draw,
                                              ErrorStack *error_stack) {
  return_rack_to_bag(game, player_index);
  if (!draw_rack_from_bag(game, player_index, rack_to_draw)) {
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "rack of ");
    string_builder_add_rack(sb, rack_to_draw, game_get_ld(game), false);
    string_builder_add_formatted_string(sb, " for player %d not in bag",
                                        player_index + 1);
    char *err_msg = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
                     err_msg);
  }
}

void play_game_history_turn(const GameHistory *game_history, Game *game,
                            int game_event_index, bool validate,
                            PlayEventsData *play_events_data,
                            ErrorStack *error_stack) {
  GameEvent *game_event =
      game_history_get_event(game_history, game_event_index);
  const GameEvent *previous_game_event = NULL;
  if (game_event_index > 0) {
    previous_game_event =
        game_history_get_event(game_history, game_event_index - 1);
  }

  const GameEvent *next_game_event = NULL;
  if (game_event_index < game_history_get_num_events(game_history) - 1) {
    next_game_event =
        game_history_get_event(game_history, game_event_index + 1);
  }

  int game_player_on_turn_index = game_get_player_on_turn_index(game);
  const int game_event_player_index = game_event_get_player_index(game_event);
  const int game_event_opp_index = 1 - game_event_player_index;

  if (validate) {
    validate_game_event_order_and_index(
        game_event, previous_game_event, next_game_event,
        game_player_on_turn_index, game_over(game), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);

  const game_event_t game_event_type = game_event_get_type(game_event);

  if (game_event_type == GAME_EVENT_END_RACK_POINTS) {
    // For this game event, the rack field is the rack of the opponent
    // that the player receives as the end rack points bonus. The actual player
    // rack is assumed to be empty.
    set_rack_from_bag_or_push_to_error_stack(
        game, 1 - game_event_player_index,
        game_event_get_const_rack(game_event), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  } else if (game_event_type != GAME_EVENT_PHONY_TILES_RETURNED) {
    // Since the previous play is already on the board, attempting to draw the
    // rack from this event might result in an error since this rack is
    // identical to the previous rack in the GCG and the rack tiles might not
    // be in the bag (for example if the phony played had an J, X, Q, or Z).
    set_rack_from_bag_or_push_to_error_stack(
        game, game_event_player_index, game_event_get_const_rack(game_event),
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  ValidatedMoves *vms = NULL;
  const char *cgp_move_string = game_event_get_cgp_move_string(game_event);
  const Equity move_score = game_event_get_move_score(game_event);
  Player *player;
  const Rack *player_rack;
  const Rack *opp_rack;
  bool all_letters_on_rack_played = false;
  switch (game_event_type) {
  case GAME_EVENT_TILE_PLACEMENT_MOVE:
  case GAME_EVENT_EXCHANGE:
  case GAME_EVENT_PASS:
    if (validate) {
      vms =
          validated_moves_create(game, game_event_player_index, cgp_move_string,
                                 true, true, true, error_stack);
      // Set the validated move in the game event immediately so
      // that the game event can take ownership of the vms.
      game_event_set_vms(game_event, vms);

      if (!error_stack_is_empty(error_stack)) {
        error_stack_push(
            error_stack, ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR,
            get_formatted_string("encountered a move validation error when "
                                 "playing game event '%s'",
                                 cgp_move_string));
        return;
      }

      // Confirm the score from the GCG matches the score from the validated
      // move
      if (move_get_score(validated_moves_get_move(vms, 0)) != move_score) {
        error_stack_push(
            error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORING_ERROR,
            get_formatted_string(
                "calculated move score (%d) does not match the move "
                "score in the GCG (%d) for move: %s",
                move_get_score(validated_moves_get_move(vms, 0)), move_score,
                game_event_get_cgp_move_string(game_event)));
        return;
      }

      Rack *known_rack_from_phonies =
          &play_events_data
               ->known_letters_from_phonies_racks[game_event_player_index];

      if (game_event_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        Rack *previously_played_letters =
            &play_events_data
                 ->previously_played_letters_racks[game_event_player_index];
        validated_moves_set_rack_to_played_letters(vms, 0,
                                                   previously_played_letters);
        rack_subtract_using_floor_zero(known_rack_from_phonies,
                                       previously_played_letters);
      } else if (game_event_type == GAME_EVENT_EXCHANGE) {
        rack_reset(known_rack_from_phonies);
      }
    } else {
      vms = game_event_get_vms(game_event);
    }

    const int player_on_turn_index = game_get_player_on_turn_index(game);
    game_set_backup_mode(game, BACKUP_MODE_GCG);
    play_move_without_drawing_tiles(validated_moves_get_move(vms, 0), game);
    game_set_backup_mode(game, BACKUP_MODE_OFF);
    all_letters_on_rack_played = rack_is_empty(
        player_get_rack(game_get_player(game, player_on_turn_index)));
    break;
  case GAME_EVENT_CHALLENGE_BONUS:
    player_add_to_score(game_get_player(game, game_event_player_index),
                        game_event_get_score_adjustment(game_event));
    break;
  case GAME_EVENT_PHONY_TILES_RETURNED:;
    if (validate) {
      const Rack *previously_played_letters =
          &play_events_data
               ->previously_played_letters_racks[game_event_player_index];
      Rack *known_rack_from_phonies =
          &play_events_data
               ->known_letters_from_phonies_racks[game_event_player_index];
      rack_union(known_rack_from_phonies, previously_played_letters);
    }
    // This event is guaranteed to immediately succeed
    // a tile placement move.
    return_phony_letters(game);
    break;
  case GAME_EVENT_TIME_PENALTY:
    if (play_events_data->played_time_penalty[game_event_player_index]) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY,
          get_formatted_string(
              "encountered a time penalty event '%s' for player '%s' when "
              "a previous time penalty event for that player has already been "
              "played",
              cgp_move_string,
              game_history_player_get_nickname(game_history,
                                               game_event_player_index)));
      return;
    }
    play_events_data->played_time_penalty[game_event_player_index] = true;
    player_add_to_score(game_get_player(game, game_event_player_index),
                        game_event_get_score_adjustment(game_event));
    break;
  case GAME_EVENT_END_RACK_PENALTY:
    if (!game_reached_max_scoreless_turns(game)) {
      // This error should have been caught earlier in game event order
      // validation
      log_fatal(
          "encountered unexpected end rack penalty before the end of the game");
    }
    if (validate &&
        play_events_data->played_end_rack_penalty[game_event_player_index]) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_END_RACK_PENALTY,
          get_formatted_string(
              "encountered an end rack penalty event '%s' for player '%s' when "
              "a previous end rack event for that player has already been "
              "played",
              cgp_move_string,
              game_history_player_get_nickname(game_history,
                                               game_event_player_index)));
      return;
    }
    play_events_data->played_end_rack_penalty[game_event_player_index] = true;
    player = game_get_player(game, game_event_player_index);
    player_rack = player_get_rack(player);
    if (rack_is_empty(player_rack)) {
      // This should be made impossible by the regex associated with this event
      log_fatal(
          "encountered an unexpected empty rack for an end rack penalty event");
    }
    player_add_to_score(
        player, calculate_end_rack_penalty(player_rack, game_get_ld(game)));
    game_start_next_player_turn(game);
    break;
  case GAME_EVENT_END_RACK_POINTS:
    player = game_get_player(game, game_event_player_index);
    player_rack = player_get_rack(player);
    opp_rack = player_get_rack(game_get_player(game, game_event_opp_index));
    if (!rack_is_empty(player_rack)) {
      log_fatal("player %s received end rack points with a nonempty rack "
                "before the game ended",
                game_history_player_get_nickname(game_history,
                                                 game_event_player_index));
    }
    if (rack_is_empty(opp_rack)) {
      log_fatal("player %s received end rack points before the game ended",
                game_history_player_get_nickname(game_history,
                                                 game_event_player_index));
    }
    if (validate && play_events_data->played_end_rack_points) {
      error_stack_push(error_stack,
                       ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_END_RACK_POINTS,
                       get_formatted_string(
                           "encountered an end rack points event when a "
                           "previous end rack event has already been played",
                           cgp_move_string));
      return;
    }
    play_events_data->played_end_rack_points = true;
    player_add_to_score(player,
                        calculate_end_rack_points(opp_rack, game_get_ld(game)));
    all_letters_on_rack_played = true;
    break;
  case GAME_EVENT_UNKNOWN:
    log_fatal("encountered unknown game event when playing game history turn");
    break;
  }

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);

  if (validate) {
    const Equity game_event_cume = game_event_get_cumulative_score(game_event);
    const Equity player_score_cume =
        player_get_score(game_get_player(game, game_event_player_index));
    if (game_event_cume != player_score_cume) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_CUMULATIVE_SCORING_ERROR,
          get_formatted_string(
              "calculated cumulative score (%d) does not match the cumulative "
              "score in the GCG (%d)",
              equity_to_int(game_event_cume),
              equity_to_int(player_score_cume)));
      return;
    }
    set_after_game_event_racks(game_history, game, game_event_index,
                               play_events_data, all_letters_on_rack_played,
                               error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  game_player_on_turn_index = game_get_player_on_turn_index(game);
  set_rack_from_bag_or_push_to_error_stack(
      game, game_player_on_turn_index,
      game_event_get_after_event_player_on_turn_rack(game_event), error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  set_rack_from_bag_or_push_to_error_stack(
      game, 1 - game_player_on_turn_index,
      game_event_get_after_event_player_off_turn_rack(game_event), error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

// Use event index 0 to go to the start of the game.
// Calling with event index N will set the game state to after the
// Nth turn has been played where n is 1-indexed.
void game_play_n_events(GameHistory *game_history, Game *game,
                        int num_events_to_play, const bool validate,
                        ErrorStack *error_stack) {
  game_reset(game);
  if (game_history_get_num_events(game_history) == 0) {
    const int player_on_turn_index = game_get_player_on_turn_index(game);
    const Rack *player_on_turn_index_rack =
        game_history_player_get_last_rack(game_history, player_on_turn_index);
    if (rack_get_dist_size(player_on_turn_index_rack) != 0) {
      set_rack_from_bag_or_push_to_error_stack(
          game, player_on_turn_index, player_on_turn_index_rack, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }
    return;
  }
  if (num_events_to_play <= 0) {
    const GameEvent *first_game_event = game_history_get_event(game_history, 0);
    set_rack_from_bag_or_push_to_error_stack(
        game, game_event_get_player_index(first_game_event),
        game_event_get_const_rack(first_game_event), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }
  const int ld_size = ld_get_size(game_get_ld(game));

  Rack stack_allocated_racks[4];
  for (size_t i = 0; i < sizeof(stack_allocated_racks) / sizeof(Rack); i++) {
    rack_set_dist_size_and_reset(&stack_allocated_racks[i], ld_size);
  }

  PlayEventsData play_events_data;
  rack_set_dist_size_and_reset(
      &play_events_data.previously_played_letters_racks[0], ld_size);
  rack_set_dist_size_and_reset(
      &play_events_data.previously_played_letters_racks[1], ld_size);
  rack_set_dist_size_and_reset(
      &play_events_data.known_letters_from_phonies_racks[0], ld_size);
  rack_set_dist_size_and_reset(
      &play_events_data.known_letters_from_phonies_racks[1], ld_size);
  play_events_data.played_end_rack_penalty[0] = false;
  play_events_data.played_end_rack_penalty[1] = false;
  play_events_data.played_time_penalty[0] = false;
  play_events_data.played_time_penalty[1] = false;
  play_events_data.played_end_rack_points = false;

  int number_of_game_events = game_history_get_num_events(game_history);
  if (num_events_to_play > number_of_game_events) {
    num_events_to_play = number_of_game_events;
  }

  for (int game_event_index = 0; game_event_index < num_events_to_play;
       game_event_index++) {
    play_game_history_turn(game_history, game, game_event_index, validate,
                           &play_events_data, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }
  for (int i = 0; i < 2; i++) {
    rack_copy(player_get_known_rack_from_phonies(game_get_player(game, i)),
              &play_events_data.known_letters_from_phonies_racks[i]);
  }
}