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
  for (int idx = 0; idx < move->tiles_length; idx++) {
    uint8_t letter = move->tiles[idx];
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    set_letter(game->gen->board, move->row_start + (move->dir * idx),
               move->col_start + ((1 - move->dir) * idx), letter);
    if (is_blanked(letter)) {
      letter = BLANK_MACHINE_LETTER;
    }
    take_letter_from_rack(game->players[game->player_on_turn_index]->rack,
                          letter);
  }
  game->gen->board->tiles_played += move->tiles_played;

  // updateAnchorsForMove
  int row = move->row_start;
  int col = move->col_start;
  if (dir_is_vertical(move->dir)) {
    row = move->col_start;
    col = move->row_start;
  }

  for (int i = col; i < move->tiles_length + col; i++) {
    update_anchors(game->gen->board, row, i, move->dir);
    if (row > 0) {
      update_anchors(game->gen->board, row - 1, i, move->dir);
    }
    if (row < BOARD_DIM - 1) {
      update_anchors(game->gen->board, row + 1, i, move->dir);
    }
  }
  if (col - 1 >= 0) {
    update_anchors(game->gen->board, row, col - 1, move->dir);
  }
  if (move->tiles_length + col < BOARD_DIM) {
    update_anchors(game->gen->board, row, move->tiles_length + col, move->dir);
  }
}

void calc_for_across(const Move *move, Game *game, int row_start, int col_start,
                     int csd) {
  for (int row = row_start; row < move->tiles_length + row_start; row++) {
    if (move->tiles[row - row_start] == PLAYED_THROUGH_MARKER) {
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
  for (int col = col_start - 1; col <= col_start + move->tiles_length; col++) {
    gen_cross_set(game->players[0]->kwg, game->gen->letter_distribution,
                  game->gen->board, row_start, col, csd, 0);
    if (game->gen->kwgs_are_distinct) {
      gen_cross_set(game->players[1]->kwg, game->gen->letter_distribution,
                    game->gen->board, row_start, col, csd, 1);
    }
  }
}

void update_cross_set_for_move(const Move *move, Game *game) {
  if (dir_is_vertical(move->dir)) {
    calc_for_across(move, game, move->row_start, move->col_start,
                    BOARD_HORIZONTAL_DIRECTION);
    transpose(game->gen->board);
    calc_for_self(move, game, move->col_start, move->row_start,
                  BOARD_VERTICAL_DIRECTION);
    transpose(game->gen->board);
  } else {
    calc_for_self(move, game, move->row_start, move->col_start,
                  BOARD_HORIZONTAL_DIRECTION);
    transpose(game->gen->board);
    calc_for_across(move, game, move->col_start, move->row_start,
                    BOARD_VERTICAL_DIRECTION);
    transpose(game->gen->board);
  }
}

void execute_exchange_move(const Move *move, Game *game) {
  for (int i = 0; i < move->tiles_played; i++) {
    take_letter_from_rack(game->players[game->player_on_turn_index]->rack,
                          move->tiles[i]);
  }
  int player_draw_index = get_player_on_turn_draw_index(game);
  draw_at_most_to_rack(game->gen->bag,
                       game->players[game->player_on_turn_index]->rack,
                       move->tiles_played, player_draw_index);
  for (int i = 0; i < move->tiles_played; i++) {
    add_letter(game->gen->bag, move->tiles[i], player_draw_index);
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
  if (move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, game);
    update_cross_set_for_move(move, game);
    game->consecutive_scoreless_turns = 0;
    game->players[game->player_on_turn_index]->score += move->score;
    draw_at_most_to_rack(
        game->gen->bag, game->players[game->player_on_turn_index]->rack,
        move->tiles_played, get_player_on_turn_draw_index(game));
    if (game->players[game->player_on_turn_index]->rack->empty) {
      standard_end_of_game_calculations(game);
    }
  } else if (move->move_type == GAME_EVENT_PASS) {
    game->consecutive_scoreless_turns++;
  } else if (move->move_type == GAME_EVENT_EXCHANGE) {
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
  for (int i = 0; i < prack->array_size; i++) {
    if (prack->array[i] > 0) {
      for (int j = 0; j < prack->array[i]; j++) {
        add_letter(game->gen->bag, i, player_draw_index);
      }
    }
  }
  reset_rack(prack);
  int ndrawn = 0;
  if (known_rack && known_rack->number_of_letters > 0) {
    for (int i = 0; i < known_rack->array_size; i++) {
      for (int j = 0; j < known_rack->array[i]; j++) {
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