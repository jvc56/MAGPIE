#include "../impl/gameplay.h"

#include "../def/cross_set_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"

#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"

#include "../../test/test_util.h"

#include "move_gen.h"

void play_move_on_board(const Move *move, Game *game) {
  // PlaceMoveTiles
  Board *board = game_get_board(game);
  for (int idx = 0; idx < get_tiles_length(move); idx++) {
    uint8_t letter = get_tile(move, idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    set_letter(board, get_row_start(move) + get_dir(move) * idx,
               get_col_start(move) + ((1 - get_dir(move)) * idx), letter);
    if (is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    take_letter_from_rack(player_get_rack(game_get_player(
                              game, game_get_player_on_turn_index(game))),
                          letter);
  }

  incrememt_tiles_played(board, move_get_tiles_played(move));

  // updateAnchorsForMove
  int row = get_row_start(move);
  int col = get_col_start(move);
  if (dir_is_vertical(get_dir(move))) {
    row = get_col_start(move);
    col = get_row_start(move);
  }

  for (int i = col; i < get_tiles_length(move) + col; i++) {
    update_anchors(board, row, i, get_dir(move));
    if (row > 0) {
      update_anchors(board, row - 1, i, get_dir(move));
    }
    if (row < BOARD_DIM - 1) {
      update_anchors(board, row + 1, i, get_dir(move));
    }
  }
  if (col - 1 >= 0) {
    update_anchors(board, row, col - 1, get_dir(move));
  }
  if (get_tiles_length(move) + col < BOARD_DIM) {
    update_anchors(board, row, get_tiles_length(move) + col, get_dir(move));
  }
}

void calc_for_across(const Move *move, Game *game, int row_start, int col_start,
                     int csd) {
  for (int row = row_start; row < get_tiles_length(move) + row_start; row++) {
    if (get_tile(move, row - row_start) == PLAYED_THROUGH_MARKER) {
      continue;
    }

    Board *board = game_get_board(game);
    bool kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    int right_col = word_edge(board, row, col_start, WORD_DIRECTION_RIGHT);
    int left_col = word_edge(board, row, col_start, WORD_DIRECTION_LEFT);
    const KWG *player0_kwg = player_get_kwg(game_get_player(game, 0));
    const LetterDistribution *ld = game_get_ld(game);
    gen_cross_set(player0_kwg, ld, board, row, right_col + 1, csd, 0);
    gen_cross_set(player0_kwg, ld, board, row, left_col - 1, csd, 0);
    gen_cross_set(player0_kwg, ld, board, row, col_start, csd, 0);
    if (!kwgs_are_shared) {
      const KWG *player1_kwg = player_get_kwg(game_get_player(game, 1));
      gen_cross_set(player1_kwg, ld, board, row, right_col + 1, csd, 1);
      gen_cross_set(player1_kwg, ld, board, row, left_col - 1, csd, 1);
      gen_cross_set(player1_kwg, ld, board, row, col_start, csd, 1);
    }
  }
}

void calc_for_self(const Move *move, Game *game, int row_start, int col_start,
                   int csd) {
  const KWG *player0_kwg = player_get_kwg(game_get_player(game, 0));
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  bool kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);

  for (int col = col_start - 1; col <= col_start + get_tiles_length(move);
       col++) {
    gen_cross_set(player0_kwg, ld, board, row_start, col, csd, 0);
  }
  if (!kwgs_are_shared) {
    const KWG *player1_kwg = player_get_kwg(game_get_player(game, 1));
    for (int col = col_start - 1; col <= col_start + get_tiles_length(move);
         col++) {
      gen_cross_set(player1_kwg, ld, board, row_start, col, csd, 1);
    }
  }
}

void update_cross_set_for_move(const Move *move, Game *game) {
  Board *board = game_get_board(game);
  if (dir_is_vertical(get_dir(move))) {
    calc_for_across(move, game, get_row_start(move), get_col_start(move),
                    BOARD_HORIZONTAL_DIRECTION);
    transpose(board);
    calc_for_self(move, game, get_col_start(move), get_row_start(move),
                  BOARD_VERTICAL_DIRECTION);
    transpose(board);
  } else {
    calc_for_self(move, game, get_row_start(move), get_col_start(move),
                  BOARD_HORIZONTAL_DIRECTION);
    transpose(board);
    calc_for_across(move, game, get_col_start(move), get_row_start(move),
                    BOARD_VERTICAL_DIRECTION);
    transpose(board);
  }
}

void execute_exchange_move(const Move *move, Game *game) {
  Rack *player_on_turn_rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  Bag *bag = game_get_bag(game);

  for (int i = 0; i < move_get_tiles_played(move); i++) {
    take_letter_from_rack(player_on_turn_rack, get_tile(move, i));
  }
  int player_draw_index = game_get_player_on_turn_draw_index(game);
  draw_at_most_to_rack(bag, player_on_turn_rack, move_get_tiles_played(move),
                       player_draw_index);
  for (int i = 0; i < move_get_tiles_played(move); i++) {
    add_letter(bag, get_tile(move, i), player_draw_index);
  }
}

void standard_end_of_game_calculations(Game *game) {
  int player_on_turn_index = game_get_player_on_turn_index(game);

  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  const Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  const LetterDistribution *ld = game_get_ld(game);

  player_increment_score(player_on_turn,
                         2 * score_on_rack(ld, player_get_rack(opponent)));
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
    backup_game(game);
  }
  const LetterDistribution *ld = game_get_ld(game);
  if (get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, game);
    update_cross_set_for_move(move, game);
    game_set_consecutive_scoreless_turns(game, 0);

    Player *player_on_turn =
        game_get_player(game, game_get_player_on_turn_index(game));
    Bag *bag = game_get_bag(game);
    Rack *player_on_turn_rack = player_get_rack(player_on_turn);

    player_increment_score(player_on_turn, get_score(move));
    draw_at_most_to_rack(bag, player_on_turn_rack, move_get_tiles_played(move),
                         game_get_player_on_turn_draw_index(game));
    if (rack_is_empty(player_on_turn_rack)) {
      standard_end_of_game_calculations(game);
    }
  } else if (get_move_type(move) == GAME_EVENT_PASS) {
    game_increment_consecutive_scoreless_turns(game);
  } else if (get_move_type(move) == GAME_EVENT_EXCHANGE) {
    execute_exchange_move(move, game);
    game_increment_consecutive_scoreless_turns(game);
  }

  if (game_get_consecutive_scoreless_turns(game) == MAX_SCORELESS_TURNS) {
    Player *player0 = game_get_player(game, 0);
    Player *player1 = game_get_player(game, 1);
    player_decrement_score(player0,
                           score_on_rack(ld, player_get_rack(player0)));
    player_decrement_score(player1,
                           score_on_rack(ld, player_get_rack(player1)));
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