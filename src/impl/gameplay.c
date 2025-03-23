#include <stdbool.h>
#include <stdint.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/gameplay_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"

#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"

#include "../str/move_string.h"
#include "move_gen.h"

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
void get_leave_for_move(const Move *move, Game *game, Rack *leave) {
  rack_copy(leave, player_get_rack(game_get_player(
                       game, game_get_player_on_turn_index(game))));
  int tiles_length = move_get_tiles_length(move);
  for (int idx = 0; idx < tiles_length; idx++) {
    uint8_t letter = move_get_tile(move, idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (get_is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    rack_take_letter(leave, letter);
  }
}

void play_move_on_board(const Move *move, Game *game) {
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
    uint8_t letter = move_get_tile(move, idx);
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

void calc_for_across(const Move *move, Game *game, int row_start, int col_start,
                     int csd) {
  for (int row = row_start; row < move_get_tiles_length(move) + row_start;
       row++) {
    if (move_get_tile(move, row - row_start) == PLAYED_THROUGH_MARKER) {
      continue;
    }

    Board *board = game_get_board(game);
    bool kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    int right_col =
        board_get_word_edge(board, row, col_start, WORD_DIRECTION_RIGHT);
    int left_col =
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

void calc_for_self(const Move *move, Game *game, int row_start, int col_start,
                   int csd) {
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

void update_cross_set_for_move(const Move *move, Game *game) {
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

void update_spots_through_main_word_vertical(const Move *move, Game *game) {
  // printf("update_spots_through_main_word_vertical\n");
  const int move_row = move_get_row_start(move);
  const int move_col = move_get_col_start(move);
  // This will make every square in the played move unusable as the start of a
  // spot except for the first square in the word.
  for (int i = 0; i < move_get_tiles_length(move); i++) {
    game_update_spots_from_square(game, move_row + i, move_col, 1,
                                  BOARD_VERTICAL_DIRECTION);
  }
  // The square below the last played tile can no longer start a vertical word.
  int row = move_row + move_get_tiles_length(move);
  if (row < BOARD_DIM) {
    game_update_spots_from_square(game, row, move_col, 1,
                                  BOARD_VERTICAL_DIRECTION);
  }
  const Board *board = game_get_board(game);
  int needed_to_reach_start = 0;
  row = move_row - 1;
  while (row >= 0) {
    if (board_is_empty(board, row, move_col)) {
      needed_to_reach_start++;
    }
    if (needed_to_reach_start > RACK_SIZE) {
      return;
    }
    game_update_spots_from_square(game, row, move_col, needed_to_reach_start,
                                  BOARD_VERTICAL_DIRECTION);
    row--;
  }
}

void update_spots_through_main_word_horizontal(const Move *move, Game *game) {
  // printf("update_spots_through_main_word_horizontal\n");
  const int move_row = move_get_row_start(move);
  const int move_col = move_get_col_start(move);
  for (int i = 0; i < move_get_tiles_length(move); i++) {
    game_update_spots_from_square(game, move_row, move_col + i, 1,
                                  BOARD_HORIZONTAL_DIRECTION);
  }
  int col = move_col + move_get_tiles_length(move);
  // The square to the right of this played tile can no longer start a
  // horizontal word.
  if (col < BOARD_DIM) {
    game_update_spots_from_square(game, move_row, col, 1,
                                  BOARD_HORIZONTAL_DIRECTION);
  }
  const Board *board = game_get_board(game);
  int needed_to_reach_start = 0;
  col = move_col - 1;
  while (col >= 0) {
    if (board_is_empty(board, move_row, col)) {
      needed_to_reach_start++;
    }
    if (needed_to_reach_start > RACK_SIZE) {
      return;
    }
    game_update_spots_from_square(game, move_row, col, needed_to_reach_start,
                                  BOARD_HORIZONTAL_DIRECTION);
    col--;
  }
}

void update_spots_through_main_word(const Move *move, Game *game) {
  // printf("update_spots_through_main_word\n");
  switch (move_get_dir(move)) {
  case BOARD_VERTICAL_DIRECTION:
    update_spots_through_main_word_vertical(move, game);
    return;
  case BOARD_HORIZONTAL_DIRECTION:
    update_spots_through_main_word_horizontal(move, game);
    return;
  }
}

void update_spots_hooking_horizontal_at_col(Game *game, int move_row,
                                            int hook_col) {
  // printf(
  //     "update_spots_hooking_horizontal_at_col... move_row: %d, hook_col:
  //     %d\n", move_row, hook_col);
  const Board *board = game_get_board(game);
  int needed_to_reach_start = 0;
  int row = move_row;
  while (row >= 0) {
    if (board_is_empty(board, row, hook_col)) {
      needed_to_reach_start++;
    }
    if (needed_to_reach_start > RACK_SIZE) {
      return;
    }
    game_update_spots_from_square(game, row, hook_col, needed_to_reach_start,
                                  BOARD_VERTICAL_DIRECTION);
    row--;
  }
}

void update_spots_hooking_horizontal(const Move *move, Game *game) {
  // printf("update_spots_hooking_horizontal\n");
  const int move_row = move_get_row_start(move);
  const int move_col_start = move_get_col_start(move);
  const int move_col_end = move_col_start + move_get_tiles_length(move) - 1;
  if (move_col_start - 1 >= 0) {
    update_spots_hooking_horizontal_at_col(game, move_row, move_col_start - 1);
  }
  if (move_col_end + 1 < BOARD_DIM) {
    update_spots_hooking_horizontal_at_col(game, move_row, move_col_end + 1);
  }
}

void update_spots_hooking_vertical_at_row(Game *game, int move_col,
                                          int hook_row) {
  // printf("update_spots_hooking_vertical_at_row... move_col: %d, hook_row:
  // %d\n",
  //        move_col, hook_row);
  const Board *board = game_get_board(game);
  int needed_to_reach_start = 0;
  int col = move_col;
  while (col >= 0) {
    if (board_is_empty(board, hook_row, col)) {
      needed_to_reach_start++;
    }
    if (needed_to_reach_start > RACK_SIZE) {
      return;
    }
    game_update_spots_from_square(game, hook_row, col, needed_to_reach_start,
                                  BOARD_HORIZONTAL_DIRECTION);
    col--;
  }
}

void update_spots_hooking_vertical(const Move *move, Game *game) {
  // printf("update_spots_hooking_vertical\n");
  int move_col = move_get_col_start(move);
  int move_row_start = move_get_row_start(move);
  int move_row_end = move_row_start + move_get_tiles_length(move) - 1;
  if (move_row_start - 1 >= 0) {
    update_spots_hooking_vertical_at_row(game, move_col, move_row_start - 1);
  }
  if (move_row_end + 1 < BOARD_DIM) {
    update_spots_hooking_vertical_at_row(game, move_col, move_row_end + 1);
  }
}

void update_spots_hooking(const Move *move, Game *game) {
  // printf("update_spots_hooking\n");
  switch (move_get_dir(move)) {
  case BOARD_VERTICAL_DIRECTION:
    update_spots_hooking_vertical(move, game);
    return;
  case BOARD_HORIZONTAL_DIRECTION:
    update_spots_hooking_horizontal(move, game);
    return;
  }
}

void update_spots_perpendicular_horizontal_at_col(Game *game, int move_row,
                                                  int col,
                                                  int last_row_updated) {
  // printf("update_spots_perpendicular_horizontal_at_col... move_row: %d, col: "
  //        "%d, last_row_updated: %d\n",
  //        move_row, col, last_row_updated);
  const Board *board = game_get_board(game);
  game_update_spots_from_square(game, move_row, col, 1,
                                BOARD_VERTICAL_DIRECTION);
  // The square below this played tile can no longer start a word.
  int row = move_row + 1;
  if (row < BOARD_DIM) {
    game_update_spots_from_square(game, row, col, 1, BOARD_VERTICAL_DIRECTION);
  }
  int needed_to_reach_start = 0;
  row = move_row - 1;
  while (row > last_row_updated) {
    assert(row >= 0);
    if (board_is_empty(board, row, col)) {
      needed_to_reach_start++;
    }
    if (needed_to_reach_start > RACK_SIZE) {
      return;
    }
    game_update_spots_from_square(game, row, col, needed_to_reach_start,
                                  BOARD_VERTICAL_DIRECTION);
    row--;
  }
}

void update_spots_perpendicular_vertical_at_row(Game *game, int move_col,
                                                int row, int last_col_updated) {
  // printf("update_spots_perpendicular_vertical_at_row... move_col: %d, row: %d, "
  //        "last_col_updated: %d\n",
  //        move_col, row, last_col_updated);
  const Board *board = game_get_board(game);
  game_update_spots_from_square(game, row, move_col, 1,
                                BOARD_HORIZONTAL_DIRECTION);
  // The square to the right of this played tile can no longer start a word.
  int col = move_col + 1;
  if (col < BOARD_DIM) {
    game_update_spots_from_square(game, row, col, 1,
                                  BOARD_HORIZONTAL_DIRECTION);
  }
  int needed_to_reach_start = 0;
  col = move_col - 1;
  while (col > last_col_updated) {
    assert(col >= 0);
    if (board_is_empty(board, row, col)) {
      needed_to_reach_start++;
    }
    if (needed_to_reach_start > RACK_SIZE) {
      return;
    }
    game_update_spots_from_square(game, row, col, needed_to_reach_start,
                                  BOARD_HORIZONTAL_DIRECTION);
    col--;
  }
}

void update_spots_perpendicular_and_parallel_horizontal(const Move *move,
                                                        Game *game) {
  // printf("update_spots_perpendicular_and_parallel_horizontal\n");
  const int move_row = move_get_row_start(move);
  const int move_col_start = move_get_col_start(move);
  const Board *board = game_get_board(game);
  int last_col_checked_with_hook_reaching_row[BOARD_DIM];
  memset(last_col_checked_with_hook_reaching_row, -1, sizeof(int) * BOARD_DIM);
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const int col = move_col_start + tile_idx;
    const uint8_t ml = move_get_tile(move, tile_idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    update_spots_perpendicular_horizontal_at_col(game, move_row, col, -1);
    int empty_row_before_tile = move_row - 1;
    while (empty_row_before_tile >= 0) {
      if (board_is_empty(board, empty_row_before_tile, col)) {
        update_spots_perpendicular_vertical_at_row(
            game, col, empty_row_before_tile,
            last_col_checked_with_hook_reaching_row[empty_row_before_tile]);
        last_col_checked_with_hook_reaching_row[empty_row_before_tile] = col;
        break;
      }
      empty_row_before_tile--;
    }
    int empty_row_after_tile = move_row + 1;
    while (empty_row_after_tile < BOARD_DIM) {
      if (board_is_empty(board, empty_row_after_tile, col)) {
        update_spots_perpendicular_vertical_at_row(
            game, col, empty_row_after_tile,
            last_col_checked_with_hook_reaching_row[empty_row_after_tile]);
        last_col_checked_with_hook_reaching_row[empty_row_after_tile] = col;
        break;
      }
      empty_row_after_tile++;
    }
  }
}

void update_spots_perpendicular_and_parallel_vertical(const Move *move,
                                                      Game *game) {
  // printf("update_spots_perpendicular_and_parallel_vertical\n");
  const int move_col = move_get_col_start(move);
  const int move_row_start = move_get_row_start(move);
  int last_row_checked_with_hook_reaching_col[BOARD_DIM];
  memset(last_row_checked_with_hook_reaching_col, -1, sizeof(int) * BOARD_DIM);
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const int row = move_row_start + tile_idx;
    const uint8_t ml = move_get_tile(move, tile_idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    update_spots_perpendicular_vertical_at_row(game, move_col, row, -1);
    int empty_col_before_tile = move_col - 1;
    while (empty_col_before_tile >= 0) {
      if (board_is_empty(game_get_board(game), row, empty_col_before_tile)) {
        update_spots_perpendicular_horizontal_at_col(
            game, row, empty_col_before_tile,
            last_row_checked_with_hook_reaching_col[empty_col_before_tile]);
        last_row_checked_with_hook_reaching_col[empty_col_before_tile] = row;
        break;
      }
      empty_col_before_tile--;
    }
    int empty_col_after_tile = move_col + 1;
    while (empty_col_after_tile < BOARD_DIM) {
      if (board_is_empty(game_get_board(game), row, empty_col_after_tile)) {
        update_spots_perpendicular_horizontal_at_col(
            game, row, empty_col_after_tile,
            last_row_checked_with_hook_reaching_col[empty_col_after_tile]);
        last_row_checked_with_hook_reaching_col[empty_col_after_tile] = row;
        break;
      }
      empty_col_after_tile++;
    }
  }
}

void update_spots_perpendicular_and_parallel(const Move *move, Game *game) {
  // printf("update_spots_perpendicular_and_parallel\n");
  switch (move_get_dir(move)) {
  case BOARD_VERTICAL_DIRECTION:
    update_spots_perpendicular_and_parallel_vertical(move, game);
    return;
  case BOARD_HORIZONTAL_DIRECTION:
    update_spots_perpendicular_and_parallel_horizontal(move, game);
    return;
  }
}

void update_spots_for_move(const Move *move, Game *game) {
  // printf("update_spots_for_move ");
  // StringBuilder *sb = string_builder_create();
  // string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game));
  // printf("%s\n", string_builder_peek(sb));
  // string_builder_destroy(sb);

  update_spots_through_main_word(move, game);
  update_spots_hooking(move, game);
  update_spots_perpendicular_and_parallel(move, game);
}

// Draws the required number of tiles to fill the rack to RACK_SIZE.
void draw_to_full_rack(Game *game, int player_index) {
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
bool rack_is_drawable(Game *game, int player_index, const Rack *rack_to_draw) {
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
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
bool draw_rack_from_bag(Game *game, int player_index,
                        const Rack *rack_to_draw) {
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  int player_draw_index = game_get_player_draw_index(game, player_index);
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  rack_copy(player_rack, rack_to_draw);
  for (int i = 0; i < dist_size; i++) {
    const int rack_number_of_letter = rack_get_letter(player_rack, i);
    for (int j = 0; j < rack_number_of_letter; j++) {
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
    const int rack_number_of_letter = rack_get_letter(rack_to_draw, i);
    for (int j = 0; j < rack_number_of_letter; j++) {
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
int draw_rack_string_from_bag(Game *game, int player_index,
                              const char *rack_string) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack *player_rack_copy = rack_create(ld_get_size(ld));
  int number_of_letters_set =
      rack_set_to_string(ld, player_rack_copy, rack_string);

  if (number_of_letters_set != -1) {
    if (!rack_is_drawable(game, player_index, player_rack_copy)) {
      number_of_letters_set = -2;
    } else {
      draw_rack_from_bag(game, player_index, player_rack_copy);
    }
  }

  rack_destroy(player_rack_copy);

  return number_of_letters_set;
}

void return_rack_to_bag(Game *game, int player_index) {
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  int player_draw_index = game_get_player_draw_index(game, player_index);
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    const int rack_number_of_letter = rack_get_letter(player_rack, i);
    for (int j = 0; j < rack_number_of_letter; j++) {
      bag_add_letter(bag, i, player_draw_index);
    }
  }
  rack_reset(player_rack);
}

void set_random_rack(Game *game, int player_index, Rack *known_rack) {
  return_rack_to_bag(game, player_index);
  if (known_rack) {
    draw_rack_from_bag(game, player_index, known_rack);
  }
  draw_to_full_rack(game, player_index);
}

void execute_exchange_move(const Move *move, Game *game, Rack *leave) {
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Rack *player_on_turn_rack =
      player_get_rack(game_get_player(game, player_on_turn_index));
  Bag *bag = game_get_bag(game);

  for (int i = 0; i < move_get_tiles_played(move); i++) {
    rack_take_letter(player_on_turn_rack, move_get_tile(move, i));
  }

  if (leave) {
    rack_copy(leave, player_on_turn_rack);
  }

  draw_to_full_rack(game, player_on_turn_index);
  int player_draw_index = game_get_player_on_turn_draw_index(game);
  for (int i = 0; i < move_get_tiles_played(move); i++) {
    bag_add_letter(bag, move_get_tile(move, i), player_draw_index);
  }
}

void standard_end_of_game_calculations(Game *game) {
  int player_on_turn_index = game_get_player_on_turn_index(game);

  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  const Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  const LetterDistribution *ld = game_get_ld(game);

  player_add_to_score(player_on_turn,
                      2 * rack_get_score(ld, player_get_rack(opponent)));
  game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
}

void draw_starting_racks(Game *game) {
  draw_to_full_rack(game, 0);
  draw_to_full_rack(game, 1);
}

// Assumes the move has been validated
// If rack_to_draw is not null, it will attempt to set the
// player rack to rack_to_draw after the play or will
// return an error if it is not possible.
// If the input leave rack is not null, it will record the leave of
// the play in the leave rack.
play_move_status_t play_move(const Move *move, Game *game,
                             const Rack *rack_to_draw, Rack *leave) {
  // printf("play_move\n");
  if (game_get_backup_mode(game) == BACKUP_MODE_SIMULATION) {
    game_backup(game);
  }
  const LetterDistribution *ld = game_get_ld(game);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  Rack *player_on_turn_rack = player_get_rack(player_on_turn);
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, game);
    if (leave) {
      rack_copy(leave, player_on_turn_rack);
    }
    update_cross_set_for_move(move, game);
    game_set_consecutive_scoreless_turns(game, 0);
    if (game_has_wmp(game)) {
      update_spots_for_move(move, game);
      // Board *board = game_get_board(game);
      // Game *game_copy = game_duplicate(game);
      // game_update_all_spots(game_copy);
      // for (int row = 0; row < BOARD_DIM; row++) {
      //   for (int col = 0; col < BOARD_DIM; col++) {
      //     for (int dir = 0; dir < 2; dir++) {
      //       for (int num_tiles = 1; num_tiles <= RACK_SIZE; num_tiles++) {
      //         const BoardSpot *spot_from_copy = board_get_writable_spot(
      //             game_get_board(game_copy), row, col, dir, num_tiles, 0);
      //         const BoardSpot *spot_from_game =
      //             board_get_writable_spot(board, row, col, dir, num_tiles, 0);
      //         if (!spot_from_copy->is_usable && !spot_from_game->is_usable) {
      //           continue;
      //         }
      //         if (spot_from_copy->is_usable && !spot_from_game->is_usable) {
      //           printf("spot at %d %d %d %d %d is usable in copy but not in "
      //                  "game\n",
      //                  row, col, dir, num_tiles, 0);
      //         }
      //         if (!spot_from_copy->is_usable && spot_from_game->is_usable) {
      //           printf("spot at %d %d %d %d %d is usable in game but not in "
      //                  "copy\n",
      //                  row, col, dir, num_tiles, 0);
      //         }
      //         assert(spot_from_copy->additional_score ==
      //                spot_from_game->additional_score);
      //         assert(spot_from_copy->playthrough_bit_rack ==
      //                spot_from_game->playthrough_bit_rack);
      //         assert(spot_from_copy->word_length ==
      //                spot_from_game->word_length);
      //         for (int i = 0; i < RACK_SIZE; i++) {
      //           assert(spot_from_copy->descending_effective_multipliers[i] ==
      //                  spot_from_game->descending_effective_multipliers[i]);
      //         }
      //       }
      //     }
      //   }
      // }
    }

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

  if (rack_to_draw) {
    if (rack_is_drawable(game, player_on_turn_index, rack_to_draw)) {
      return_rack_to_bag(game, player_on_turn_index);
      draw_rack_from_bag(game, player_on_turn_index, rack_to_draw);
    } else {
      return PLAY_MOVE_STATUS_RACK_TO_DRAW_NOT_IN_BAG;
    }
  }

  if (game_get_consecutive_scoreless_turns(game) ==
      game_get_max_scoreless_turns(game)) {
    Player *player0 = game_get_player(game, 0);
    Player *player1 = game_get_player(game, 1);
    player_add_to_score(player0, -rack_get_score(ld, player_get_rack(player0)));
    player_add_to_score(player1, -rack_get_score(ld, player_get_rack(player1)));
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  }
  game_start_next_player_turn(game);
  return PLAY_MOVE_STATUS_SUCCESS;
}

void return_phony_tiles(Game *game) {
  game_unplay_last_move(game);
  game_start_next_player_turn(game);
  game_increment_consecutive_scoreless_turns(game);
}

void generate_moves_for_game(Game *game, int thread_index,
                             MoveList *move_list) {
  Player *player_on_turn =
      game_get_player(game, game_get_player_on_turn_index(game));
  generate_moves(game, player_get_move_record_type(player_on_turn),
                 player_get_move_sort_type(player_on_turn), thread_index,
                 move_list, /*override_kwg=*/NULL);
}

Move *get_top_equity_move(Game *game, int thread_index, MoveList *move_list) {
  generate_moves(game, MOVE_RECORD_BEST, MOVE_SORT_EQUITY, thread_index,
                 move_list, /*override_kwg=*/NULL);
  return move_list_get_move(move_list, 0);
}

void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter,
                         int player_draw_index) {
  bag_draw_letter(bag, letter, player_draw_index);
  rack_add_letter(rack, letter);
}
