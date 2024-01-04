#include "../def/cross_set_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"

#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"

#include "move_gen.h"

void play_move_on_board(const Move *move, Game *game) {
  // PlaceMoveTiles
  Board *board = game_get_board(game);
  for (int idx = 0; idx < move_get_tiles_length(move); idx++) {
    uint8_t letter = move_get_tile(move, idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    board_set_letter(
        board, move_get_row_start(move) + move_get_dir(move) * idx,
        move_get_col_start(move) + ((1 - move_get_dir(move)) * idx), letter);
    if (get_is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    rack_take_letter(player_get_rack(game_get_player(
                         game, game_get_player_on_turn_index(game))),
                     letter);
  }

  board_increment_tiles_played(board, move_get_tiles_played(move));

  // updateAnchorsForMove
  int row = move_get_row_start(move);
  int col = move_get_col_start(move);
  if (board_is_dir_vertical(move_get_dir(move))) {
    row = move_get_col_start(move);
    col = move_get_row_start(move);
  }

  for (int i = col; i < move_get_tiles_length(move) + col; i++) {
    board_update_anchors(board, row, i, move_get_dir(move));
    if (row > 0) {
      board_update_anchors(board, row - 1, i, move_get_dir(move));
    }
    if (row < BOARD_DIM - 1) {
      board_update_anchors(board, row + 1, i, move_get_dir(move));
    }
  }
  if (col - 1 >= 0) {
    board_update_anchors(board, row, col - 1, move_get_dir(move));
  }
  if (move_get_tiles_length(move) + col < BOARD_DIM) {
    board_update_anchors(board, row, move_get_tiles_length(move) + col,
                         move_get_dir(move));
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
                    move_get_col_start(move), BOARD_HORIZONTAL_DIRECTION);
    board_transpose(board);
    calc_for_self(move, game, move_get_col_start(move),
                  move_get_row_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  } else {
    calc_for_self(move, game, move_get_row_start(move),
                  move_get_col_start(move), BOARD_HORIZONTAL_DIRECTION);
    board_transpose(board);
    calc_for_across(move, game, move_get_col_start(move),
                    move_get_row_start(move), BOARD_VERTICAL_DIRECTION);
    board_transpose(board);
  }
}

void draw_at_most_to_rack(Bag *bag, Rack *rack, int n, int player_draw_index) {
  while (n > 0 && !bag_is_empty(bag)) {
    rack_add_letter(rack, bag_draw_random_letter(bag, player_draw_index));
    n--;
  }
}

void execute_exchange_move(const Move *move, Game *game) {
  Rack *player_on_turn_rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  Bag *bag = game_get_bag(game);

  for (int i = 0; i < move_get_tiles_played(move); i++) {
    rack_take_letter(player_on_turn_rack, move_get_tile(move, i));
  }
  int player_draw_index = game_get_player_on_turn_draw_index(game);
  draw_at_most_to_rack(bag, player_on_turn_rack, move_get_tiles_played(move),
                       player_draw_index);
  for (int i = 0; i < move_get_tiles_played(move); i++) {
    bag_add_letter(bag, move_get_tile(move, i), player_draw_index);
  }
}

void standard_end_of_game_calculations(Game *game) {
  int player_on_turn_index = game_get_player_on_turn_index(game);

  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  const Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  const LetterDistribution *ld = game_get_ld(game);

  player_increment_score(player_on_turn,
                         2 * rack_get_score(ld, player_get_rack(opponent)));
  game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
}

void draw_starting_racks(Game *game) {
  Bag *bag = game_get_bag(game);
  draw_at_most_to_rack(bag, player_get_rack(game_get_player(game, 0)),
                       RACK_SIZE, game_get_player_draw_index(game, 0));
  draw_at_most_to_rack(bag, player_get_rack(game_get_player(game, 1)),
                       RACK_SIZE, game_get_player_draw_index(game, 1));
}

void play_move(const Move *move, Game *game) {
  if (game_get_backup_mode(game) == BACKUP_MODE_SIMULATION) {
    game_backup(game);
  }
  const LetterDistribution *ld = game_get_ld(game);
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, game);
    update_cross_set_for_move(move, game);
    game_set_consecutive_scoreless_turns(game, 0);

    Player *player_on_turn =
        game_get_player(game, game_get_player_on_turn_index(game));
    Bag *bag = game_get_bag(game);
    Rack *player_on_turn_rack = player_get_rack(player_on_turn);

    player_increment_score(player_on_turn, move_get_score(move));
    draw_at_most_to_rack(bag, player_on_turn_rack, move_get_tiles_played(move),
                         game_get_player_on_turn_draw_index(game));
    if (rack_is_empty(player_on_turn_rack)) {
      standard_end_of_game_calculations(game);
    }
  } else if (move_get_type(move) == GAME_EVENT_PASS) {
    game_increment_consecutive_scoreless_turns(game);
  } else if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    execute_exchange_move(move, game);
    game_increment_consecutive_scoreless_turns(game);
  }

  if (game_get_consecutive_scoreless_turns(game) == MAX_SCORELESS_TURNS) {
    Player *player0 = game_get_player(game, 0);
    Player *player1 = game_get_player(game, 1);
    player_decrement_score(player0,
                           rack_get_score(ld, player_get_rack(player0)));
    player_decrement_score(player1,
                           rack_get_score(ld, player_get_rack(player1)));
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  }

  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    game_start_next_player_turn(game);
  }
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

void set_random_rack(Game *game, int pidx, Rack *known_rack) {
  Bag *bag = game_get_bag(game);
  Rack *prack = player_get_rack(game_get_player(game, pidx));
  int ntiles = rack_get_total_letters(prack);
  int player_draw_index = game_get_player_draw_index(game, pidx);
  // always try to fill rack if possible.
  if (ntiles < RACK_SIZE) {
    ntiles = RACK_SIZE;
  }
  // throw in existing rack, then redraw from the bag.
  for (int i = 0; i < rack_get_dist_size(prack); i++) {
    if (rack_get_letter(prack, i) > 0) {
      for (int j = 0; j < rack_get_letter(prack, i); j++) {
        bag_add_letter(bag, i, player_draw_index);
      }
    }
  }
  rack_reset(prack);
  int ndrawn = 0;
  if (known_rack && rack_get_total_letters(known_rack) > 0) {
    for (int i = 0; i < rack_get_dist_size(known_rack); i++) {
      for (int j = 0; j < rack_get_letter(known_rack, i); j++) {
        draw_letter_to_rack(bag, prack, i, player_draw_index);
        ndrawn++;
      }
    }
  }
  draw_at_most_to_rack(bag, prack, ntiles - ndrawn, player_draw_index);
}