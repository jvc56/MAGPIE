#include <assert.h>
#include <dirent.h>
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
#include "../../src/ent/game.h"
#include "../../src/ent/inference_results.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/validated_move.h"
#include "../../src/impl/config.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"

#include "../../src/str/game_string.h"
#include "../../src/str/inference_string.h"
#include "../../src/str/move_string.h"
#include "../../src/str/rack_string.h"

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
    board_set_letter(board, row, i, ALPHABET_EMPTY_SQUARE_MARKER);
  }
  char letter[2];
  letter[1] = '\0';
  for (size_t i = 0; i < string_length(row_content); i++) {
    if (row_content[i] != ' ') {
      letter[0] = row_content[i];
      board_set_letter(board, row, i, ld_hl_to_ml(ld, letter));
      board_increment_tiles_played(board, 1);
    }
  }
}

// this test func only works for single-char alphabets
uint64_t string_to_cross_set(const LetterDistribution *ld,
                             const char *letters) {
  if (strings_equal(letters, TRIVIAL_CROSS_SET_STRING)) {
    return TRIVIAL_CROSS_SET;
  }
  uint64_t c = 0;
  char letter[2];
  letter[1] = '\0';

  for (size_t i = 0; i < string_length(letters); i++) {
    letter[0] = letters[i];
    c |= get_cross_set_bit(ld_hl_to_ml(ld, letter));
  }
  return c;
}

void load_and_exec_config_or_die(Config *config, const char *cmd) {
  config_load_status_t status = config_load_command(config, cmd);
  if (status != CONFIG_LOAD_STATUS_SUCCESS) {
    log_fatal("load config failed with status %d: %s\n", status, cmd);
  }
  config_execute_command(config);
  ErrorStatus *error_status = config_get_error_status(config);
  if (!error_status_get_success(error_status)) {
    error_status_log_warn_if_failed(error_status);
    abort();
  }
}

char *cross_set_to_string(const LetterDistribution *ld, uint64_t input) {
  StringBuilder *css_builder = string_builder_create();
  for (int i = 0; i < MAX_ALPHABET_SIZE; ++i) {
    if (input & ((uint64_t)1 << i)) {
      string_builder_add_string(css_builder, ld_ml_to_hl(ld, i));
    }
  }
  char *result = string_builder_dump(css_builder, NULL);
  string_builder_destroy(css_builder);
  return result;
}

// Loads path with a default test data path value.
// To specify a different path, use load_and_exec_config_or_die
// after calling this function.
Config *config_create_or_die(const char *cmd) {
  Config *config = config_create_default();
  load_and_exec_config_or_die(config, "set -path " DEFAULT_TEST_DATA_PATH);
  load_and_exec_config_or_die(config, cmd);
  return config;
}

Config *config_create_default_test(void) {
  Config *config = config_create_default();
  load_and_exec_config_or_die(config, "set -path " DEFAULT_TEST_DATA_PATH);
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

SortedMoveList *sorted_move_list_create(MoveList *ml) {
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

void sorted_move_list_destroy(SortedMoveList *sorted_move_list) {
  if (!sorted_move_list) {
    return;
  }
  free(sorted_move_list->moves);
  free(sorted_move_list);
}

void print_move_list(const Board *board, const LetterDistribution *ld,
                     const SortedMoveList *sml, int move_list_length) {
  StringBuilder *move_list_string = string_builder_create();
  for (int i = 0; i < move_list_length; i++) {
    string_builder_add_move(move_list_string, board, sml->moves[i], ld);
    string_builder_add_string(move_list_string, "\n");
  }
  printf("%s\n", string_builder_peek(move_list_string));
  string_builder_destroy(move_list_string);
}

void print_game(Game *game, MoveList *move_list) {
  StringBuilder *game_string = string_builder_create();
  string_builder_add_game(game_string, game, move_list);
  printf("%s\n", string_builder_peek(game_string));
  string_builder_destroy(game_string);
}

void print_cgp(const Game *game) {
  char *cgp = game_get_cgp(game, true);
  printf("%s\n", cgp);
  free(cgp);
}

void print_rack(const Rack *rack, const LetterDistribution *ld) {
  if (!rack) {
    printf("(null)\n");
    return;
  }
  StringBuilder *rack_sb = string_builder_create();
  string_builder_add_rack(rack_sb, rack, ld);
  printf("%s", string_builder_peek(rack_sb));
  string_builder_destroy(rack_sb);
}

void print_inference(const LetterDistribution *ld,
                     const Rack *target_played_tiles,
                     InferenceResults *inference_results) {
  StringBuilder *inference_string = string_builder_create();
  string_builder_add_inference(inference_string, ld, inference_results,
                               target_played_tiles);
  printf("%s\n", string_builder_peek(inference_string));
  string_builder_destroy(inference_string);
}

void sort_and_print_move_list(const Board *board, const LetterDistribution *ld,
                              MoveList *ml) {
  SortedMoveList *sml = sorted_move_list_create(ml);
  print_move_list(board, ld, sml, sml->count);
  sorted_move_list_destroy(sml);
}

void play_top_n_equity_move(Game *game, int n) {
  MoveList *move_list = move_list_create(n + 1);
  generate_moves(game, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0, move_list);
  SortedMoveList *sorted_move_list = sorted_move_list_create(move_list);
  play_move(sorted_move_list->moves[n], game, NULL);
  sorted_move_list_destroy(sorted_move_list);
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
void assert_boards_are_equal(Board *b1, Board *b2) {
  assert(board_get_transposed(b1) == board_get_transposed(b2));
  assert(board_get_tiles_played(b1) == board_get_tiles_played(b2));
  for (int t = 0; t < 2; t++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      if (t == 0) {
        assert(board_get_number_of_row_anchors(b1, row, 0) ==
               board_get_number_of_row_anchors(b2, row, 0));
        assert(board_get_number_of_row_anchors(b1, row, 1) ==
               board_get_number_of_row_anchors(b2, row, 1));
      }
      for (int col = 0; col < BOARD_DIM; col++) {
        assert(board_get_letter(b1, row, col) ==
               board_get_letter(b2, row, col));
        assert(board_get_bonus_square(b1, row, col) ==
               board_get_bonus_square(b2, row, col));
        for (int dir = 0; dir < 2; dir++) {
          assert(board_get_anchor(b1, row, col, dir) ==
                 board_get_anchor(b2, row, col, dir));
          assert(board_get_is_cross_word(b1, row, col, dir) ==
                 board_get_is_cross_word(b2, row, col, dir));
          // For now, assume all boards tested in this method
          // share the same lexicon
          for (int cross_index = 0; cross_index < 2; cross_index++) {
            assert(board_get_cross_set(b1, row, col, dir, cross_index) ==
                   board_get_cross_set(b2, row, col, dir, cross_index));
            assert(board_get_cross_score(b1, row, col, dir, cross_index) ==
                   board_get_cross_score(b2, row, col, dir, cross_index));
          }
        }
      }
    }
    board_transpose(b1);
    board_transpose(b2);
  }
}

void assert_move(Game *game, MoveList *move_list, const SortedMoveList *sml,
                 int move_index, const char *expected_move_string) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  StringBuilder *move_string = string_builder_create();
  Move *move;
  if (sml) {
    move = sml->moves[move_index];
  } else {
    move = move_list_get_move(move_list, move_index);
  }
  string_builder_add_move(move_string, board, move, ld);
  if (!strings_equal(string_builder_peek(move_string), expected_move_string)) {
    fprintf(stderr, "moves are not equal\ngot: >%s<\nexp: >%s<\n",
            string_builder_peek(move_string), expected_move_string);
    assert(0);
  }
  string_builder_destroy(move_string);
}

void assert_players_are_equal(const Player *p1, const Player *p2,
                              bool check_scores) {
  // For games ending in consecutive zeros, scores are checked elsewhere
  if (check_scores) {
    assert(player_get_score(p1) == player_get_score(p2));
  }
}

void assert_games_are_equal(Game *g1, Game *g2, bool check_scores) {
  assert(game_get_consecutive_scoreless_turns(g1) ==
         game_get_consecutive_scoreless_turns(g2));
  assert(game_get_game_end_reason(g1) == game_get_game_end_reason(g2));

  int g1_player_on_turn_index = game_get_player_on_turn_index(g1);

  const Player *g1_player_on_turn =
      game_get_player(g1, g1_player_on_turn_index);
  const Player *g1_player_not_on_turn =
      game_get_player(g1, 1 - g1_player_on_turn_index);

  int g2_player_on_turn_index = game_get_player_on_turn_index(g2);

  const Player *g2_player_on_turn =
      game_get_player(g2, g2_player_on_turn_index);
  const Player *g2_player_not_on_turn =
      game_get_player(g2, 1 - g2_player_on_turn_index);

  assert_players_are_equal(g1_player_on_turn, g2_player_on_turn, check_scores);
  assert_players_are_equal(g1_player_not_on_turn, g2_player_not_on_turn,
                           check_scores);

  Board *board1 = game_get_board(g1);
  Board *board2 = game_get_board(g2);

  Bag *bag1 = game_get_bag(g1);
  Bag *bag2 = game_get_bag(g2);

  assert_boards_are_equal(board1, board2);
  assert_bags_are_equal(bag1, bag2, ld_get_size(game_get_ld(g1)));
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

void fifo_create(const char *fifo_name) {
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

// Board layout test helpers

void assert_board_layout_error(const char *data_path,
                               const char *board_layout_name,
                               board_layout_load_status_t expected_status) {
  BoardLayout *bl = board_layout_create();
  board_layout_load_status_t actual_status =
      board_layout_load(bl, data_path, board_layout_name);
  board_layout_destroy(bl);
  if (actual_status != expected_status) {
    printf("board layout load statuses do not match: %d != %d", expected_status,
           actual_status);
  }
  assert(actual_status == expected_status);
}

BoardLayout *board_layout_create_for_test(const char *data_path,
                                          const char *board_layout_name) {
  BoardLayout *bl = board_layout_create();
  board_layout_load_status_t actual_status =
      board_layout_load(bl, data_path, board_layout_name);
  if (actual_status != BOARD_LAYOUT_LOAD_STATUS_SUCCESS) {
    printf("board layout load failure for %s: %d\n", board_layout_name,
           actual_status);
  }
  assert(actual_status == BOARD_LAYOUT_LOAD_STATUS_SUCCESS);
  return bl;
}

void load_game_with_test_board(Game *game, const char *data_path,
                               const char *board_layout_name) {
  BoardLayout *bl = board_layout_create_for_test(data_path, board_layout_name);
  board_apply_layout(bl, game_get_board(game));
  game_reset(game);
  board_layout_destroy(bl);
}

char *remove_parentheses(const char *input) {
  int len = strlen(input);
  char *result = (char *)malloc_or_die((len + 1) * sizeof(char));
  int j = 0;
  for (int i = 0; i < len; i++) {
    if (input[i] != '(' && input[i] != ')') {
      result[j++] = input[i];
    }
  }
  result[j] = '\0';
  return result;
}

void assert_validated_and_generated_moves(Game *game, const char *rack_string,
                                          const char *move_position,
                                          const char *move_tiles,
                                          int move_score,
                                          bool play_move_on_board) {
  Player *player = game_get_player(game, game_get_player_on_turn_index(game));
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(1);

  rack_set_to_string(game_get_ld(game), player_rack, rack_string);

  generate_moves_for_game(game, 0, move_list);
  char *gen_move_string;
  if (strings_equal(move_position, "exch")) {
    gen_move_string = get_formatted_string("(exch %s)", move_tiles);
  } else {
    gen_move_string =
        get_formatted_string("%s %s %d", move_position, move_tiles, move_score);
  }
  assert_move(game, move_list, NULL, 0, gen_move_string);
  free(gen_move_string);

  char *move_tiles_no_parens = remove_parentheses(move_tiles);
  char *vm_move_string;
  if (strings_equal(move_position, "exch")) {
    vm_move_string = get_formatted_string("ex.%s", move_tiles_no_parens);
  } else {
    vm_move_string =
        get_formatted_string("%s.%s", move_position, move_tiles_no_parens);
  }
  free(move_tiles_no_parens);

  ValidatedMoves *vms =
      validated_moves_create(game, 0, vm_move_string, false, true, false);
  free(vm_move_string);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);

  if (play_move_on_board) {
    play_move(move_list_get_move(move_list, 0), game, NULL);
  }

  validated_moves_destroy(vms);
  move_list_destroy(move_list);
}

ValidatedMoves *assert_validated_move_success(Game *game, const char *cgp_str,
                                              const char *move_str,
                                              int player_index,
                                              bool allow_phonies,
                                              bool allow_playthrough) {
  load_cgp_or_die(game, cgp_str);
  ValidatedMoves *vms = validated_moves_create(
      game, player_index, move_str, allow_phonies, true, allow_playthrough);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  return vms;
}

void assert_game_matches_cgp(const Game *game, const char *expected_cgp,
                             bool write_player_on_turn_first) {
  char *actual_cgp = game_get_cgp(game, write_player_on_turn_first);

  StringSplitter *split_cgp = split_string_by_whitespace(expected_cgp, true);
  char *expected_cgp_without_options =
      string_splitter_join(split_cgp, 0, 4, " ");
  string_splitter_destroy(split_cgp);
  assert_strings_equal(actual_cgp, expected_cgp_without_options);
  free(actual_cgp);
  free(expected_cgp_without_options);
}

void assert_stats_are_equal(const Stat *s1, const Stat *s2) {
  assert(stat_get_num_unique_samples(s1) == stat_get_num_unique_samples(s2));
  assert(stat_get_num_samples(s1) == stat_get_num_samples(s2));
  assert(within_epsilon(stat_get_mean(s1), stat_get_mean(s2)));
  assert(within_epsilon(stat_get_variance(s1), stat_get_variance(s2)));
}

void assert_moves_are_equal(const Move *m1, const Move *m2) {
  assert(move_get_type(m1) == move_get_type(m2));
  assert(move_get_row_start(m1) == move_get_row_start(m2));
  assert(move_get_col_start(m1) == move_get_col_start(m2));
  assert(move_get_tiles_played(m1) == move_get_tiles_played(m2));
  assert(move_get_tiles_length(m1) == move_get_tiles_length(m2));
  assert(move_get_score(m1) == move_get_score(m2));
  assert(move_get_dir(m1) == move_get_dir(m2));
  assert(within_epsilon(move_get_equity(m1), move_get_equity(m2)));
  int tiles_length = move_get_tiles_length(m1);
  for (int i = 0; i < tiles_length; i++) {
    assert(move_get_tile(m1, i) == move_get_tile(m2, i));
  }
}

void assert_simmed_plays_are_equal(const SimmedPlay *sp1, const SimmedPlay *sp2,
                                   int max_plies) {
  assert(simmed_play_get_id(sp1) == simmed_play_get_id(sp2));
  assert(simmed_play_get_ignore(sp1) == simmed_play_get_ignore(sp2));
  assert_moves_are_equal(simmed_play_get_move(sp1), simmed_play_get_move(sp2));

  for (int i = 0; i < max_plies; i++) {
    assert_stats_are_equal(simmed_play_get_score_stat(sp1, i),
                           simmed_play_get_score_stat(sp2, i));
    assert_stats_are_equal(simmed_play_get_bingo_stat(sp1, i),
                           simmed_play_get_bingo_stat(sp2, i));
  }

  assert_stats_are_equal(simmed_play_get_equity_stat(sp1),
                         simmed_play_get_equity_stat(sp2));
  assert_stats_are_equal(simmed_play_get_win_pct_stat(sp1),
                         simmed_play_get_win_pct_stat(sp2));
}

// NOT THREAD SAFE
void assert_sim_results_equal(SimResults *sr1, SimResults *sr2) {
  sim_results_sort_plays_by_win_rate(sr1);
  sim_results_sort_plays_by_win_rate(sr2);
  assert(sim_results_get_max_plies(sr1) == sim_results_get_max_plies(sr2));
  assert(sim_results_get_number_of_plays(sr1) ==
         sim_results_get_number_of_plays(sr2));
  assert(sim_results_get_iteration_count(sr1) ==
         sim_results_get_iteration_count(sr2));
  assert(within_epsilon(sim_results_get_zval(sr1), sim_results_get_zval(sr2)));
  assert(sim_results_get_node_count(sr1) == sim_results_get_node_count(sr2));
  for (int i = 0; i < sim_results_get_number_of_plays(sr1); i++) {
    assert_simmed_plays_are_equal(sim_results_get_simmed_play(sr1, i),
                                  sim_results_get_simmed_play(sr2, i),
                                  sim_results_get_max_plies(sr1));
  }
}