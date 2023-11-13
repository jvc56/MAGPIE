#include "test_util.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/board.h"
#include "../src/config.h"
#include "../src/constants.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/infer.h"
#include "../src/klv.h"
#include "../src/move.h"
#include "../src/rack.h"
#include "../src/util.h"
#include "test_constants.h"

int within_epsilon(double a, double b) { return fabs(a - b) < 1e-6; }

double get_leave_value_for_rack(KLV *klv, Rack *rack) {
  return get_leave_value(klv, rack);
}

void generate_moves_for_game(Game *game) {
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
}

void generate_leaves_for_game(Game *game, int add_exchange) {
  Generator *gen = game->gen;
  Player *player = game->players[game->player_on_turn_index];
  init_leave_map(gen->leave_map, player->rack);
  if (player->rack->number_of_letters < RACK_SIZE) {
    set_current_value(
        gen->leave_map,
        get_leave_value(player->strategy_params->klv, player->rack));
  } else {
    set_current_value(gen->leave_map, INITIAL_TOP_MOVE_EQUITY);
  }

  // Set the best leaves and maybe add exchanges.
  generate_exchange_moves(gen, player, 0, 0, add_exchange);
}

void look_up_bingos_for_game(Game *game) {
  Generator *gen = game->gen;
  Player *player = game->players[game->player_on_turn_index];
  gen->number_of_bingos = 0;
  look_up_bingos(gen, player);
}

SortedMoveList *create_sorted_move_list(MoveList *ml) {
  int number_of_moves = ml->count;
  SortedMoveList *sorted_move_list = malloc_or_die((sizeof(SortedMoveList)));
  sorted_move_list->moves = malloc_or_die((sizeof(Move *)) * (number_of_moves));
  sorted_move_list->count = number_of_moves;
  for (int i = number_of_moves - 1; i >= 0; i--) {
    Move *move = pop_move(ml);
    sorted_move_list->moves[i] = move;
  }
  return sorted_move_list;
}

void destroy_sorted_move_list(SortedMoveList *sorted_move_list) {
  free(sorted_move_list->moves);
  free(sorted_move_list);
}

void print_anchor_list(Generator *gen) {
  for (int i = 0; i < gen->anchor_list->count; i++) {
    Anchor *anchor = gen->anchor_list->anchors[i];
    int row = anchor->row;
    int col = anchor->col;
    char *dir = "Horizontal";
    if (anchor->vertical) {
      row = anchor->col;
      col = anchor->row;
      dir = "Vertical";
    }
    printf("Anchor %d: Row %d, Col %d, %s, %0.4f, %d\n", i, row, col, dir,
           anchor->highest_possible_equity, anchor->last_anchor_col);
  }
}

void print_move_list(Board *board, LetterDistribution *letter_distribution,
                     SortedMoveList *sml, int move_list_length) {
  StringBuilder *move_list_string = create_string_builder();
  for (int i = 0; i < move_list_length; i++) {
    string_builder_add_move(board, sml->moves[0], letter_distribution,
                            move_list_string);
    string_builder_add_string(move_list_string, "\n", 0);
  }
  printf("%s\n", string_builder_peek(move_list_string));
  destroy_string_builder(move_list_string);
}

void print_game(Game *game) {
  StringBuilder *game_string = create_string_builder();
  string_builder_add_game(game, game_string);
  printf("%s\n", string_builder_peek(game_string));
  destroy_string_builder(game_string);
}

void print_inference(Inference *inference, Rack *rack) {
  StringBuilder *inference_string = create_string_builder();
  string_builder_add_inference(inference, rack, inference_string);
  printf("%s\n", string_builder_peek(inference_string));
  destroy_string_builder(inference_string);
}

void sort_and_print_move_list(Board *board,
                              LetterDistribution *letter_distribution,
                              MoveList *ml) {
  SortedMoveList *sml = create_sorted_move_list(ml);
  print_move_list(board, letter_distribution, sml, sml->count);
  destroy_sorted_move_list(sml);
}

void play_top_n_equity_move(Game *game, int n) {
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  SortedMoveList *sorted_move_list =
      create_sorted_move_list(game->gen->move_list);
  play_move(game, sorted_move_list->moves[n]);
  destroy_sorted_move_list(sorted_move_list);
  reset_move_list(game->gen->move_list);
}

void draw_rack_to_string(Bag *bag, Rack *rack, char *letters,
                         LetterDistribution *letter_distribution) {
  uint8_t mls[1000];
  int num_mls =
      str_to_machine_letters(letter_distribution, letters, false, mls);
  for (int i = 0; i < num_mls; i++) {
    draw_letter_to_rack(bag, rack, mls[i]);
  }
}

int count_newlines(const char *str) {
  int count = 0;

  while (*str != '\0') {
    if (*str == '\n') {
      count++;
    }
    str++;
  }
  return count;
}

void assert_strings_equal(const char *str1, const char *str2) {
  if (!strings_equal(str1, str2)) {
    fprintf(stderr, "strings are not equal:\n>%s<\n>%s<\n", str1, str2);
    assert(0);
  }
}

void assert_move(Game *game, SortedMoveList *sml, int move_index,
                 char *expected_move_string) {
  StringBuilder *move_string = create_string_builder();
  Move *move;
  if (sml) {
    move = sml->moves[move_index];
  } else {
    move = game->gen->move_list->moves[move_index];
  }
  string_builder_add_move(game->gen->board, move,
                          game->gen->letter_distribution, move_string);
  if (!strings_equal(string_builder_peek(move_string), expected_move_string)) {
    fprintf(stderr, "moves are not equal\ngot: >%s<\nexp: >%s",
            string_builder_peek(move_string), expected_move_string);
    assert(0);
  }
  destroy_string_builder(move_string);
}

void assert_bingo_found(Generator *gen, const char *expected_bingo) {
  for (int i = 0; i < gen->number_of_bingos; i++) {
    StringBuilder *bingo = create_string_builder();
    for (int j = 0; j < RACK_SIZE; j++) {
      uint8_t uv_tile = gen->rack_bingos[i][j];
      string_builder_add_user_visible_letter(gen->letter_distribution, uv_tile,
                                             0, bingo);
    }
    if (strings_equal(string_builder_peek(bingo), expected_bingo)) {
      destroy_string_builder(bingo);
      return;
    }
    destroy_string_builder(bingo);
  }
  fprintf(stderr, "expected bingo not found: %s\n", expected_bingo);
  assert(0);
}