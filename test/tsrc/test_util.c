#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/def/board_defs.h"
#include "../../src/def/config_defs.h"
#include "../../src/def/game_defs.h"
#include "../../src/def/letter_distribution_defs.h"
#include "../../src/def/move_defs.h"

#include "../../src/ent/bag.h"
#include "../../src/ent/board.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/inference_results.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"

#include "../../src/str/game_string.h"
#include "../../src/str/inference_string.h"
#include "../../src/str/move_string.h"

#include "../../src/util/log.h"
#include "../../src/util/string_util.h"
#include "../../src/util/util.h"

#include "test_constants.h"
#include "test_util.h"

bool within_epsilon(double a, double b) { return fabs(a - b) < 1e-6; }

// This test function only works for single-char alphabets
void set_row(Game *game, int row, const char *row_content) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  for (int i = 0; i < BOARD_DIM; i++) {
    board_set_letter(board, row, i, ALPHABET_EMPTY_SQUARE_MARKER, false);
  }
  char letter[2];
  letter[1] = '\0';
  for (size_t i = 0; i < string_length(row_content); i++) {
    if (row_content[i] != ' ') {
      letter[0] = row_content[i];
      board_set_letter(board, row, i, ld_hl_to_ml(ld, letter), false);
      board_increment_tiles_played(board, 1);
    }
  }
}

void load_config_or_die(Config *config, const char *cmd) {
  config_load_status_t status = config_load(config, cmd);
  if (status != CONFIG_LOAD_STATUS_SUCCESS) {
    log_fatal("load config failed with status %d: %s\n", status, cmd);
  }
}

Config *create_config_or_die(const char *cmd) {
  Config *config = config_create_default();
  load_config_or_die(config, cmd);
  return config;
}

// Comparison function for qsort
int compare_moves_for_sml(const void *a, const void *b) {
  const Move *move_a = *(const Move **)a;
  const Move *move_b = *(const Move **)b;

  // Compare moves based on their scores
  return move_get_score(move_b) - move_get_score(move_a);
}

// Function to sort the moves in the SortedMoveList
void resort_sorted_move_list_by_score(SortedMoveList *sml) {
  qsort(sml->moves, sml->count, sizeof(Move *), compare_moves_for_sml);
}

SortedMoveList *create_sorted_move_list(MoveList *ml) {
  int number_of_moves = move_list_get_count(ml);
  SortedMoveList *sorted_move_list = malloc_or_die((sizeof(SortedMoveList)));
  sorted_move_list->moves = malloc_or_die((sizeof(Move *)) * (number_of_moves));
  sorted_move_list->count = number_of_moves;
  for (int i = number_of_moves - 1; i >= 0; i--) {
    Move *move = move_list_pop_move(ml);
    sorted_move_list->moves[i] = move;
  }
  return sorted_move_list;
}

void destroy_sorted_move_list(SortedMoveList *sorted_move_list) {
  if (!sorted_move_list) {
    return;
  }
  free(sorted_move_list->moves);
  free(sorted_move_list);
}

void print_move_list(const Board *board, const LetterDistribution *ld,
                     const SortedMoveList *sml, int move_list_length) {
  StringBuilder *move_list_string = create_string_builder();
  for (int i = 0; i < move_list_length; i++) {
    string_builder_add_move(board, sml->moves[i], ld, move_list_string);
    string_builder_add_string(move_list_string, "\n");
  }
  printf("%s\n", string_builder_peek(move_list_string));
  destroy_string_builder(move_list_string);
}

void print_game(Game *game, MoveList *move_list) {
  StringBuilder *game_string = create_string_builder();
  string_builder_add_game(game, move_list, game_string);
  printf("%s\n", string_builder_peek(game_string));
  destroy_string_builder(game_string);
}

void print_inference(const LetterDistribution *ld,
                     const Rack *target_played_tiles,
                     InferenceResults *inference_results) {
  StringBuilder *inference_string = create_string_builder();
  string_builder_add_inference(ld, inference_results, target_played_tiles,
                               inference_string);
  printf("%s\n", string_builder_peek(inference_string));
  destroy_string_builder(inference_string);
}

void sort_and_print_move_list(const Board *board, const LetterDistribution *ld,
                              MoveList *ml) {
  SortedMoveList *sml = create_sorted_move_list(ml);
  print_move_list(board, ld, sml, sml->count);
  destroy_sorted_move_list(sml);
}

void play_top_n_equity_move(Game *game, int n) {
  MoveList *move_list = move_list_create(n + 1);
  generate_moves(game, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0,
                 player_get_rack(game_get_player(
                     game, game_get_player_on_turn_index(game))),
                 move_list);
  SortedMoveList *sorted_move_list = create_sorted_move_list(move_list);
  play_move(sorted_move_list->moves[n], game);
  destroy_sorted_move_list(sorted_move_list);
  move_list_destroy(move_list);
}

void load_cgp_or_die(Game *game, const char *cgp) {
  cgp_parse_status_t cgp_parse_status = game_load_cgp(game, cgp);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    log_fatal("cgp load failed with %d\n", cgp_parse_status);
  }
}

void draw_rack_to_string(const LetterDistribution *ld, Bag *bag, Rack *rack,
                         char *letters, int player_index) {

  uint8_t mls[MAX_BAG_SIZE];
  int num_mls = ld_str_to_mls(ld, letters, false, mls, MAX_BAG_SIZE);
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
  if (rack_get_total_letters(expected_rack) !=
      rack_get_total_letters(actual_rack)) {
    printf("num letters: %d != %d\n", rack_get_total_letters(expected_rack),
           rack_get_total_letters(actual_rack));
    return false;
  }
  if (rack_get_dist_size(expected_rack) != rack_get_dist_size(actual_rack)) {
    printf("sizes: %d != %d\n", rack_get_dist_size(expected_rack),
           rack_get_dist_size(actual_rack));
    return false;
  }
  for (int i = 0; i < rack_get_dist_size(expected_rack); i++) {
    if (rack_get_letter(expected_rack, i) != rack_get_letter(actual_rack, i)) {
      printf("different: %d: %d != %d\n", i, rack_get_letter(expected_rack, i),
             rack_get_letter(actual_rack, i));
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

  int b1_number_of_tiles_remaining = bag_get_tiles(b1_copy);
  int b2_number_of_tiles_remaining = bag_get_tiles(b2_copy);

  assert(b1_number_of_tiles_remaining == b2_number_of_tiles_remaining);

  Rack *rack = rack_create(rack_array_size);

  for (int i = 0; i < b1_number_of_tiles_remaining; i++) {
    uint8_t letter = bag_draw_random_letter(b1_copy, 0);
    rack_add_letter(rack, letter);
  }

  for (int i = 0; i < b2_number_of_tiles_remaining; i++) {
    uint8_t letter = bag_draw_random_letter(b2_copy, 0);
    assert(rack_get_letter(rack, letter) > 0);
    rack_take_letter(rack, letter);
  }

  assert(rack_is_empty(rack));

  bag_destroy(b1_copy);
  bag_destroy(b2_copy);
  rack_destroy(rack);
}

// Assumes b1 and b2 use the same lexicon and therefore
// does not compare the cross set index of 1.
void assert_boards_are_equal(const Board *b1, const Board *b2) {
  assert(board_get_tiles_played(b1) == board_get_tiles_played(b2));
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      assert(board_get_letter(b1, row, col, false) ==
             board_get_letter(b2, row, col, false));
      assert(board_get_bonus_square(b1, row, col, false) ==
             board_get_bonus_square(b2, row, col, false));
      for (int dir = 0; dir < 2; dir++) {
        assert(board_get_anchor(b1, row, col, dir, false) ==
               board_get_anchor(b2, row, col, dir, false));
        // For now, assume all boards tested in this method
        // share the same lexicon
        assert(board_get_cross_set(b1, row, col, dir, 0, false) ==
               board_get_cross_set(b2, row, col, dir, 0, false));
        assert(board_get_cross_score(b1, row, col, dir, 0, false) ==
               board_get_cross_score(b2, row, col, dir, 0, false));
      }
    }
  }
}

void assert_move(Game *game, MoveList *move_list, const SortedMoveList *sml,
                 int move_index, const char *expected_move_string) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  StringBuilder *move_string = create_string_builder();
  Move *move;
  if (sml) {
    move = sml->moves[move_index];
  } else {
    move = move_list_get_move(move_list, move_index);
  }
  string_builder_add_move(board, move, ld, move_string);
  if (!strings_equal(string_builder_peek(move_string), expected_move_string)) {
    fprintf(stderr, "moves are not equal\ngot: >%s<\nexp: >%s<\n",
            string_builder_peek(move_string), expected_move_string);
    assert(0);
  }
  destroy_string_builder(move_string);
}

char *get_test_filename(const char *filename) {
  return get_formatted_string("%s%s", TESTDATA_FILEPATH, filename);
}

void delete_file(const char *filename) {
  errno = 0;
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

  errno = 0;
  result = mkfifo(fifo_name, 0666); // Read/write permissions for everyone
  if (result < 0) {
    int error_number = errno;
    if (error_number != EEXIST) {
      log_fatal("mkfifo %s for with %d\n", fifo_name, error_number);
    }
  }
}

void delete_fifo(const char *fifo_name) { unlink(fifo_name); }