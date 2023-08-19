#include <stdio.h>

#include "../src/config.h"
#include "../src/game.h"
#include "../src/letter_distribution.h"
#include "../src/move.h"
#include "../src/movegen.h"
#include "../src/util.h"

#include "gen_all_test.h"
#include "test_util.h"

void write_test_move_to_end_of_buffer(char *buf, Move *m,
                                      LetterDistribution *letter_distribution) {
  if (m->move_type == MOVE_TYPE_PASS) {
    write_string_to_end_of_buffer(buf, "pass");
    return;
  }

  // Write the move type
  write_int_to_end_of_buffer(buf, m->move_type);
  write_char_to_end_of_buffer(buf, ',');

  // Write the coords
  if (m->move_type == MOVE_TYPE_EXCHANGE) {
    write_string_to_end_of_buffer(buf, "1A");
  } else if (m->vertical) {
    write_char_to_end_of_buffer(buf, m->col_start + 'A');
    write_int_to_end_of_buffer(buf, m->row_start + 1);
  } else {
    write_int_to_end_of_buffer(buf, m->row_start + 1);
    write_char_to_end_of_buffer(buf, m->col_start + 'A');
  }
  write_char_to_end_of_buffer(buf, ',');

  if (m->move_type == MOVE_TYPE_PLAY) {
    for (int i = 0; i < m->tiles_length; i++) {
      uint8_t tile = m->tiles[i];
      uint8_t print_tile = tile;
      if (tile == PLAYED_THROUGH_MARKER) {
        write_char_to_end_of_buffer(buf, ASCII_PLAYED_THROUGH);
      } else {
        write_user_visible_letter_to_end_of_buffer(buf, letter_distribution,
                                                   print_tile);
      }
    }
  } else {
    for (int i = 0; i < m->tiles_played; i++) {
      if (m->tiles[i] != BLANK_MACHINE_LETTER) {
        write_user_visible_letter_to_end_of_buffer(buf, letter_distribution,
                                                   m->tiles[i]);
      }
    }
    for (int i = 0; i < m->tiles_played; i++) {
      if (m->tiles[i] == BLANK_MACHINE_LETTER) {
        write_user_visible_letter_to_end_of_buffer(buf, letter_distribution,
                                                   m->tiles[i]);
      }
    }
  }

  write_char_to_end_of_buffer(buf, ',');
  write_int_to_end_of_buffer(buf, m->score);
}

void test_gen_all(Config *config) {
  Game *game = create_game(config);
  reset_game(game);
  load_cgp(game, config->cgp);

  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  char csv_move[30];
  for (int i = 0; i < ml->count; i++) {
    reset_string(csv_move);
    write_test_move_to_end_of_buffer(csv_move, ml->moves[i],
                                     config->letter_distribution);
    printf("%s\n", csv_move);
  }

  destroy_game(game);
}