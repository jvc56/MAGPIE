#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../def/rack_defs.h"

#include "../src/str/string_util.h"

#include "../src/ent/board.h"
#include "../src/ent/config.h"
#include "../src/ent/game.h"
#include "../src/ent/inference.h"
#include "../src/ent/klv.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/impl/gameplay.h"
#include "../src/util/log.h"
#include "../src/util/util.h"

#include "test_constants.h"
#include "test_util.h"

bool within_epsilon(double a, double b) { return fabs(a - b) < 1e-6; }

double get_leave_value_for_rack(const KLV *klv, const Rack *rack) {
  return get_leave_value(klv, rack);
}

void load_config_or_die(Config *config, const char *cmd) {
  config_load_status_t status = load_config(config, cmd);
  if (status != CONFIG_LOAD_STATUS_SUCCESS) {
    log_fatal("load config failed with status %d\n", status);
  }
}

void generate_moves_for_game(Game *game) {
  Generator *gen = game_get_gen(game);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Bag *bag = gen_get_bag(gen);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  Rack *player_on_turn_rack = player_get_rack(player_on_turn);
  Rack *opponent_rack = player_get_rack(opponent);
  generate_moves(opponent_rack, gen, player_on_turn,
                 get_tiles_remaining(bag) >= RACK_SIZE,
                 player_get_move_record_type(player_on_turn),
                 player_get_move_sort_type(player_on_turn), true);
}

void generate_leaves_for_game(Game *game, bool add_exchange) {
  Generator *gen = game_get_gen(game);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);

  Rack *player_on_turn_rack = player_get_rack(player_on_turn);
  LeaveMap *leave_map = gen_get_leave_map(gen);

  init_leave_map(player_on_turn_rack, leave_map);
  if (get_number_of_letters(player_on_turn_rack) < RACK_SIZE) {
    set_current_value(leave_map, get_leave_value(player_get_klv(player_on_turn),
                                                 player_on_turn_rack));
  } else {
    set_current_value(leave_map, INITIAL_TOP_MOVE_EQUITY);
  }

  // Set the best leaves and maybe add exchanges.
  generate_exchange_moves(gen, player_on_turn, 0, 0, add_exchange);
}

SortedMoveList *create_sorted_move_list(MoveList *ml) {
  int number_of_moves = move_list_get_count(ml);
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

void print_move_list(const Board *board,
                     const LetterDistribution *letter_distribution,
                     const SortedMoveList *sml, int move_list_length) {
  StringBuilder *move_list_string = create_string_builder();
  for (int i = 0; i < move_list_length; i++) {
    string_builder_add_move(board, sml->moves[0], letter_distribution,
                            move_list_string);
    string_builder_add_string(move_list_string, "\n");
  }
  printf("%s\n", string_builder_peek(move_list_string));
  destroy_string_builder(move_list_string);
}

void print_game(const Game *game) {
  StringBuilder *game_string = create_string_builder();
  string_builder_add_game(game, game_string);
  printf("%s\n", string_builder_peek(game_string));
  destroy_string_builder(game_string);
}

void print_inference(const Inference *inference, const Rack *rack) {
  StringBuilder *inference_string = create_string_builder();
  string_builder_add_inference(inference, rack, inference_string);
  printf("%s\n", string_builder_peek(inference_string));
  destroy_string_builder(inference_string);
}

void sort_and_print_move_list(const Board *board,
                              const LetterDistribution *letter_distribution,
                              MoveList *ml) {
  SortedMoveList *sml = create_sorted_move_list(ml);
  print_move_list(board, letter_distribution, sml, sml->count);
  destroy_sorted_move_list(sml);
}

void play_top_n_equity_move(Game *game, int n) {
  Generator *gen = game_get_gen(game);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Bag *bag = gen_get_bag(gen);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  Rack *player_on_turn_rack = player_get_rack(player_on_turn);
  Rack *opponent_rack = player_get_rack(opponent);
  MoveList *move_list = gen_get_move_list(gen);

  generate_moves(opponent_rack, gen, player_on_turn,
                 get_tiles_remaining(bag) >= RACK_SIZE,
                 player_get_move_record_type(player_on_turn),
                 player_get_move_sort_type(player_on_turn), true);

  SortedMoveList *sorted_move_list = create_sorted_move_list(move_list);
  play_move(sorted_move_list->moves[n], game);
  destroy_sorted_move_list(sorted_move_list);
  reset_move_list(move_list);
}

void draw_rack_to_string(const LetterDistribution *letter_distribution,
                         Bag *bag, Rack *rack, char *letters,
                         int player_index) {

  uint8_t mls[MAX_BAG_SIZE];
  int num_mls = str_to_machine_letters(letter_distribution, letters, false, mls,
                                       MAX_BAG_SIZE);
  for (int i = 0; i < num_mls; i++) {
    // For tests we assume that player_index == player_draw_index
    // since starting_player_index will always be 0.
    draw_letter_to_rack(bag, rack, mls[i], player_index);
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

bool equal_rack(const Rack *expected_rack, const Rack *actual_rack) {
  if (rack_is_empty(expected_rack) != rack_is_empty(actual_rack)) {
    printf("not empty\n");
    return false;
  }
  if (get_number_of_letters(expected_rack) !=
      get_number_of_letters(actual_rack)) {
    printf("num letters: %d != %d\n", get_number_of_letters(expected_rack),
           get_number_of_letters(actual_rack));
    return false;
  }
  if (get_array_size(expected_rack) != get_array_size(actual_rack)) {
    printf("sizes: %d != %d\n", get_array_size(expected_rack),
           get_array_size(actual_rack));
    return false;
  }
  for (int i = 0; i < get_array_size(expected_rack); i++) {
    if (get_number_of_letter(expected_rack, i) !=
        get_number_of_letter(actual_rack, i)) {
      printf("different: %d: %d != %d\n", i,
             get_number_of_letter(expected_rack, i),
             get_number_of_letter(actual_rack, i));
      return false;
    }
  }
  return true;
}

void assert_strings_equal(const char *str1, const char *str2) {
  if (!strings_equal(str1, str2)) {
    if (str1 && !str2) {
      fprintf(stderr, "strings are not equal:\n>%s<\n>%s<\n", str1, "(NULL)");
    } else if (!str1 && str2) {
      fprintf(stderr, "strings are not equal:\n>%s<\n>%s<\n", "(NULL)", str2);
    } else {
      fprintf(stderr, "strings are not equal:\n>%s<\n>%s<\n", str1, str2);
    }
    assert(0);
  }
}

void assert_bags_are_equal(const Bag *b1, const Bag *b2, int rack_array_size) {
  Bag *b1_copy = bag_duplicate(b1);
  Bag *b2_copy = bag_duplicate(b2);

  int b1_number_of_tiles_remaining = get_tiles_remaining(b1_copy);
  int b2_number_of_tiles_remaining = get_tiles_remaining(b2_copy);

  assert(b1_number_of_tiles_remaining == b2_number_of_tiles_remaining);

  Rack *rack = create_rack(rack_array_size);

  for (int i = 0; i < b1_number_of_tiles_remaining; i++) {
    uint8_t letter = draw_random_letter(b1_copy, 0);
    add_letter_to_rack(rack, letter);
  }

  for (int i = 0; i < b2_number_of_tiles_remaining; i++) {
    uint8_t letter = draw_random_letter(b2_copy, 0);
    assert(get_number_of_letter(rack, letter) > 0);
    take_letter_from_rack(rack, letter);
  }

  assert(rack_is_empty(rack));

  destroy_bag(b1_copy);
  destroy_bag(b2_copy);
  destroy_rack(rack);
}

// Assumes b1 and b2 use the same lexicon and therefore
// does not compare the cross set index of 1.
void assert_boards_are_equal(const Board *b1, const Board *b2) {
  assert(get_transpose(b1) == get_transpose(b2));
  assert(get_tiles_played(b1) == get_tiles_played(b2));
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      assert(get_letter(b1, row, col) == get_letter(b2, row, col));
      assert(get_bonus_square(b1, row, col) == get_bonus_square(b2, row, col));
      for (int dir = 0; dir < 2; dir++) {
        assert(get_anchor(b1, row, col, dir) == get_anchor(b2, row, col, dir));
        // For now, assume all boards tested in this method
        // share the same lexicon
        assert(get_cross_set(b1, row, col, dir, 0) ==
               get_cross_set(b2, row, col, dir, 0));
        assert(get_cross_score(b1, row, col, dir, 0) ==
               get_cross_score(b2, row, col, dir, 0));
      }
    }
  }
}

void assert_move(const Game *game, const SortedMoveList *sml, int move_index,
                 const char *expected_move_string) {

  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  MoveList *move_list = gen_get_move_list(gen);
  Board *board = gen_get_board(gen);

  StringBuilder *move_string = create_string_builder();
  Move *move;
  if (sml) {
    move = sml->moves[move_index];
  } else {
    move = move_list_get_move(move_list, move_index);
  }
  string_builder_add_move(board, move, ld, move_string);
  if (!strings_equal(string_builder_peek(move_string), expected_move_string)) {
    fprintf(stderr, "moves are not equal\ngot: >%s<\nexp: >%s",
            string_builder_peek(move_string), expected_move_string);
    assert(0);
  }
  destroy_string_builder(move_string);
}

char *get_test_filename(const char *filename) {
  return get_formatted_string("%s%s", TESTDATA_FILEPATH, filename);
}

void delete_file(const char *filename) {
  int result = remove(filename);
  if (result != 0) {
    int error_number = errno;
    if (error_number != ENOENT) {
      log_fatal("remove %s failed with code: %d\n", filename, error_number);
    }
  }
}

void reset_file(const char *filename) { fclose(fopen(filename, "w")); }

void create_fifo(const char *fifo_name) {
  int result;

  result = mkfifo(fifo_name, 0666); // Read/write permissions for everyone
  if (result < 0) {
    int error_number = errno;
    if (error_number != EEXIST) {
      log_fatal("mkfifo %s for with %d\n", fifo_name, error_number);
    }
  }
}

void delete_fifo(const char *fifo_name) { unlink(fifo_name); }