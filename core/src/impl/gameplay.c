#include <stdio.h>

#include "board.h"
#include "cross_set.h"
#include "game.h"
#include "game_event.h"
#include "gameplay.h"
#include "move.h"
#include "movegen.h"
#include "rack.h"

void draw_at_most_to_rack(Bag *bag, Rack *rack, int n, int player_draw_index) {
  while (n > 0 && !bag_is_empty(bag)) {
    add_letter_to_rack(rack, draw_random_letter(bag, player_draw_index));
    n--;
  }
}

void play_move_on_board(const Move *move, Game *game) {
  // PlaceMoveTiles
  for (int idx = 0; idx <get_tiles_length(move); idx++) {
    uint8_t letter =get_tile(move, idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    set_letter(game->gen->board,get_row_start(move) + get_dir(move) * idx),
              get_col_start(move) + ((1 -get_dir(move)) * idx), letter);
    if (is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    take_letter_from_rack(game->players[game->player_on_turn_index]->rack,
                          letter);
  }

  incrememt_tiles_played(game->gen->board,get_tiles_played(move));

  // updateAnchorsForMove
  int row =get_row_start(move);
  int col =get_col_start(move);
  if (dir_is_verticalget_dir(move))) {
    row =get_col_start(move);
    col =get_row_start(move);
  }

  for (int i = col; i <get_tiles_length(move) + col; i++) {
    update_anchors(game->gen->board, row, i,get_dir(move));
    if (row > 0) {
      update_anchors(game->gen->board, row - 1, i,get_dir(move));
    }
    if (row < BOARD_DIM - 1) {
      update_anchors(game->gen->board, row + 1, i,get_dir(move));
    }
  }
  if (col - 1 >= 0) {
    update_anchors(game->gen->board, row, col - 1,get_dir(move));
  }
  if get_tiles_length(move) + col < BOARD_DIM) {
    update_anchors(game->gen->board, row,get_tiles_length(move) + col,get_dir(move));
  }
}

void calc_for_across(const Move *move, Game *game, int row_start, int col_start,
                     int csd) {
  for (int row = row_start; row <get_tiles_length(move) + row_start; row++) {
    if get_tile(move, row - row_start) == PLAYED_THROUGH_MARKER) {
      continue;
    }

    int right_col =
        word_edge(game->gen->board, row, col_start, WORD_DIRECTION_RIGHT);
    int left_col =
        word_edge(game->gen->board, row, col_start, WORD_DIRECTION_LEFT);
    gen_cross_set(game->players[0]->kwg, game->gen->letter_distribution,
                  game->gen->board, row, right_col + 1, csd, 0);
    gen_cross_set(game->players[0]->kwg, game->gen->letter_distribution,
                  game->gen->board, row, left_col - 1, csd, 0);
    gen_cross_set(game->players[0]->kwg, game->gen->letter_distribution,
                  game->gen->board, row, col_start, csd, 0);
    if (game->gen->kwgs_are_distinct) {
      gen_cross_set(game->players[1]->kwg, game->gen->letter_distribution,
                    game->gen->board, row, right_col + 1, csd, 1);
      gen_cross_set(game->players[1]->kwg, game->gen->letter_distribution,
                    game->gen->board, row, left_col - 1, csd, 1);
      gen_cross_set(game->players[1]->kwg, game->gen->letter_distribution,
                    game->gen->board, row, col_start, csd, 1);
    }
  }
}

void calc_for_self(const Move *move, Game *game, int row_start, int col_start,
                   int csd) {
  for (int col = col_start - 1; col <= col_start +get_tiles_length(move); col++) {
    gen_cross_set(game->players[0]->kwg, game->gen->letter_distribution,
                  game->gen->board, row_start, col, csd, 0);
    if (game->gen->kwgs_are_distinct) {
      gen_cross_set(game->players[1]->kwg, game->gen->letter_distribution,
                    game->gen->board, row_start, col, csd, 1);
    }
  }
}

void update_cross_set_for_move(const Move *move, Game *game) {
  if (dir_is_verticalget_dir(move))) {
    calc_for_across(move, game,get_row_start(move),get_col_start(move),
                    BOARD_HORIZONTAL_DIRECTION);
    transpose(game->gen->board);
    calc_for_self(move, game,get_col_start(move),get_row_start(move),
                  BOARD_VERTICAL_DIRECTION);
    transpose(game->gen->board);
  } else {
    calc_for_self(move, game,get_row_start(move),get_col_start(move),
                  BOARD_HORIZONTAL_DIRECTION);
    transpose(game->gen->board);
    calc_for_across(move, game,get_col_start(move),get_row_start(move),
                    BOARD_VERTICAL_DIRECTION);
    transpose(game->gen->board);
  }
}

void execute_exchange_move(const Move *move, Game *game) {
  for (int i = 0; i <get_tiles_played(move); i++) {
    take_letter_from_rack(game->players[game->player_on_turn_index]->rack,
                         get_tile(move, i));
  }
  int player_draw_index = get_player_on_turn_draw_index(game);
  draw_at_most_to_rack(game->gen->bag,
                       game->players[game->player_on_turn_index]->rack,
                      get_tiles_played(move), player_draw_index);
  for (int i = 0; i <get_tiles_played(move); i++) {
    add_letter(game->gen->bag,get_tile(move, i), player_draw_index);
  }
}

void standard_end_of_game_calculations(Game *game) {
  game->players[game->player_on_turn_index]->score +=
      2 * score_on_rack(game->gen->letter_distribution,
                        game->players[1 - game->player_on_turn_index]->rack);
  game->game_end_reason = GAME_END_REASON_STANDARD;
}

void draw_starting_racks(Game *game) {
  draw_at_most_to_rack(game->gen->bag, game->players[0]->rack, RACK_SIZE,
                       get_player_draw_index(game, 0));
  draw_at_most_to_rack(game->gen->bag, game->players[1]->rack, RACK_SIZE,
                       get_player_draw_index(game, 1));
}

void play_move(const Move *move, Game *game) {
  if (game->backup_mode == BACKUP_MODE_SIMULATION) {
    backup_game(game);
  }
  if get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, game);
    update_cross_set_for_move(move, game);
    game->consecutive_scoreless_turns = 0;
    game->players[game->player_on_turn_index]->score +=get_score(move);
    draw_at_most_to_rack(
        game->gen->bag, game->players[game->player_on_turn_index]->rack,
       get_tiles_played(move), get_player_on_turn_draw_index(game));
    if (game->players[game->player_on_turn_index]->rack->empty) {
      standard_end_of_game_calculations(game);
    }
  } else if get_move_type(move) == GAME_EVENT_PASS) {
    game->consecutive_scoreless_turns++;
  } else if get_move_type(move) == GAME_EVENT_EXCHANGE) {
    execute_exchange_move(move, game);
    game->consecutive_scoreless_turns++;
  }

  if (game->consecutive_scoreless_turns == MAX_SCORELESS_TURNS) {
    game->players[0]->score -=
        score_on_rack(game->gen->letter_distribution, game->players[0]->rack);
    game->players[1]->score -=
        score_on_rack(game->gen->letter_distribution, game->players[1]->rack);
    game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
  }

  if (game->game_end_reason == GAME_END_REASON_NONE) {
    game->player_on_turn_index = 1 - game->player_on_turn_index;
  }
}

void set_random_rack(Game *game, int pidx, Rack *known_rack) {
  Rack *prack = game->players[pidx]->rack;
  int ntiles = prack->number_of_letters;
  int player_draw_index = get_player_draw_index(game, pidx);
  // always try to fill rack if possible.
  if (ntiles < RACK_SIZE) {
    ntiles = RACK_SIZE;
  }
  // throw in existing rack, then redraw from the bag.
  for (int i = 0; i < pget_array_size(rack); i++) {
    if (pget_number_of_letter(rack, i) > 0) {
      for (int j = 0; j < pget_number_of_letter(rack, i); j++) {
        add_letter(game->gen->bag, i, player_draw_index);
      }
    }
  }
  reset_rack(prack);
  int ndrawn = 0;
  if (known_rack && known_rack->number_of_letters > 0) {
    for (int i = 0; i < known_get_array_size(rack); i++) {
      for (int j = 0; j < known_get_number_of_letter(rack, i); j++) {
        draw_letter_to_rack(game->gen->bag, prack, i, player_draw_index);
        ndrawn++;
      }
    }
  }
  draw_at_most_to_rack(game->gen->bag, prack, ntiles - ndrawn,
                       player_draw_index);
}

Move *get_top_equity_move(Game *game) {
  reset_move_list(game->gen->move_list);
  generate_moves(game->players[1 - game->player_on_turn_index]->rack, game->gen,
                 game->players[game->player_on_turn_index],
                 get_tiles_remaining(game->gen->bag) >= RACK_SIZE,
                 MOVE_RECORD_BEST, MOVE_SORT_EQUITY, true);
  return game->gen->move_list->moves[0];
}