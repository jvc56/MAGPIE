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
#include "../ent/game_history.h"
#include "../ent/klv.h"
#include "../ent/leave_list.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"

#include "move_gen.h"

#include "../util/string_util.h"

double get_leave_value_for_move(const KLV *klv, const Move *move, Rack *rack) {
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

// Draws at most n random tiles from the bag to the rack.
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
  const int dist_size = rack_get_dist_size(player_rack);
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
  const int dist_size = rack_get_dist_size(player_rack);
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
  const int dist_size = rack_get_dist_size(player_rack);

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
// If leave is not null, it will set the rack to leave
// after the play is made.
play_move_status_t play_move(const Move *move, Game *game,
                             const Rack *rack_to_draw, Rack *leave) {
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

  if (game_get_consecutive_scoreless_turns(game) == MAX_SCORELESS_TURNS) {
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
                 move_list);
}

Move *get_top_equity_move(Game *game, int thread_index, MoveList *move_list) {
  generate_moves(game, MOVE_RECORD_BEST, MOVE_SORT_EQUITY, thread_index,
                 move_list);
  return move_list_get_move(move_list, 0);
}

void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter,
                         int player_draw_index) {
  bag_draw_letter(bag, letter, player_draw_index);
  rack_add_letter(rack, letter);
}
