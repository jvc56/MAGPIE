#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
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
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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

void calc_for_across(const Move *move, Game *game, int row_start, int col_start,
                     int csd) {
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
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  int player_draw_index = game_get_player_draw_index(game, player_index);
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  rack_copy(player_rack, rack_to_draw);
  for (int i = 0; i < dist_size; i++) {
    const int8_t rack_number_of_letter = rack_get_letter(player_rack, i);
    for (int8_t j = 0; j < rack_number_of_letter; j++) {
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
    const int8_t rack_number_of_letter = rack_get_letter(rack_to_draw, i);
    for (int8_t j = 0; j < rack_number_of_letter; j++) {
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

void return_rack_to_bag(const Game *game, const int player_index) {
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  int player_draw_index = game_get_player_draw_index(game, player_index);
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    const int8_t rack_number_of_letter = rack_get_letter(player_rack, i);
    for (int8_t j = 0; j < rack_number_of_letter; j++) {
      bag_add_letter(bag, i, player_draw_index);
    }
  }
  rack_reset(player_rack);
}

void set_random_rack(const Game *game, const int player_index,
                     const Rack *known_rack) {
  return_rack_to_bag(game, player_index);
  if (known_rack) {
    draw_rack_from_bag(game, player_index, known_rack);
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

void standard_end_of_game_calculations(Game *game) {
  int player_on_turn_index = game_get_player_on_turn_index(game);

  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  const Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  const LetterDistribution *ld = game_get_ld(game);

  player_add_to_score(player_on_turn,
                      2 * rack_get_score(ld, player_get_rack(opponent)));
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
  if (game_get_consecutive_scoreless_turns(game) ==
      game_get_max_scoreless_turns(game)) {
    Player *player0 = game_get_player(game, 0);
    Player *player1 = game_get_player(game, 1);
    player_add_to_score(player0, -rack_get_score(ld, player_get_rack(player0)));
    player_add_to_score(player1, -rack_get_score(ld, player_get_rack(player1)));
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
  if (game_get_consecutive_scoreless_turns(game) ==
      game_get_max_scoreless_turns(game)) {
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  }
  game_start_next_player_turn(game);
}

void return_phony_letters(Game *game) {
  game_unplay_last_move(game);
  game_start_next_player_turn(game);
  game_increment_consecutive_scoreless_turns(game);
}

// Overwrites the passed in values for the following MoveGenArgs fields:
// - move_record_type (with the player on turn's record type)
// - move_sort_type (with the player on turn's sort type)
// - overrride_kwg (with NULL)
void generate_moves_for_game(const MoveGenArgs *args) {
  const Player *player_on_turn =
      game_get_player(args->game, game_get_player_on_turn_index(args->game));

  const MoveGenArgs args_with_overwritten_record_and_sort = {
      .game = args->game,
      .move_list = args->move_list,
      .move_record_type = player_get_move_record_type(player_on_turn),
      .move_sort_type = player_get_move_sort_type(player_on_turn),
      .override_kwg = NULL,
      .thread_index = args->thread_index,
      .max_equity_diff = args->max_equity_diff,
  };

  generate_moves(&args_with_overwritten_record_and_sort);
}

Move *get_top_equity_move(Game *game, int thread_index, MoveList *move_list) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = thread_index,
      .max_equity_diff = 0,
  };
  generate_moves(&args);
  return move_list_get_move(move_list, 0);
}

bool moves_are_similar(const Move *move1, const Move *move2, int dist_size) {
  if (!(move_get_dir(move1) == move_get_dir(move2) &&
        move_get_col_start(move1) == move_get_col_start(move2) &&
        move_get_row_start(move1) == move_get_row_start(move2))) {
    return false;
  }
  if (!(move_get_tiles_played(move1) == move_get_tiles_played(move2) &&
        move_get_tiles_length(move1) == move_get_tiles_length(move2))) {
    return false;
  }

  // Create a rack from move1, then subtract the rack from move2. The final
  // rack should have all zeroes.
  Rack similar_plays_rack;
  rack_set_dist_size_and_reset(&similar_plays_rack, dist_size);
  for (int i = 0; i < move_get_tiles_length(move1); i++) {
    MachineLetter tile = move_get_tile(move1, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (get_is_blanked(ml)) {
      ml = BLANK_MACHINE_LETTER;
    }
    rack_add_letter(&similar_plays_rack, ml);
  }

  for (int i = 0; i < move_get_tiles_length(move2); i++) {
    MachineLetter tile = move_get_tile(move2, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (get_is_blanked(ml)) {
      ml = BLANK_MACHINE_LETTER;
    }
    rack_take_letter(&similar_plays_rack, ml);
  }

  for (int i = 0; i < dist_size; i++) {
    if (rack_get_letter(&similar_plays_rack, i) != 0) {
      return false;
    }
  }
  return true;
}