#include "test_util.h"

#include "../src/def/board_defs.h"
#include "../src/def/cross_set_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/anchor.h"
#include "../src/ent/bag.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/board.h"
#include "../src/ent/board_layout.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/klv_csv.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/wmp.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/gcg.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmp_move_gen.h"
#include "../src/str/game_string.h"
#include "../src/str/inference_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_EPSILON 1e-6

// Global variable for the timeout function.
jmp_buf env;

void assert_equal_at_equity_resolution(double a, double b) {
  assert(double_to_equity(a) == double_to_equity(b));
}

bool within_epsilon(double a, double b) { return fabs(a - b) < TEST_EPSILON; }

// This test function only works for single-char alphabets
void set_row(const Game *game, const int row, const char *row_content) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  for (int i = 0; i < BOARD_DIM; i++) {
    board_set_letter(board, row, i, ALPHABET_EMPTY_SQUARE_MARKER);
  }
  char letter[2];
  letter[1] = '\0';
  size_t content_length = string_length(row_content);
  for (size_t i = 0; i < content_length; i++) {
    if (row_content[i] != ' ') {
      letter[0] = row_content[i];
      board_set_letter(board, row, (int)i, ld_hl_to_ml(ld, letter));
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

void set_thread_control_status_to_start(ThreadControl *thread_control) {
  if (!thread_control_is_ready_for_new_command(thread_control)) {
    thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  }
  if (!thread_control_is_started(thread_control)) {
    thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  }
}

void load_and_exec_config_or_die(Config *config, const char *cmd) {
  ErrorStack *error_stack = error_stack_create();
  set_thread_control_status_to_start(config_get_thread_control(config));
  config_load_command(config, cmd, error_stack);
  error_code_t status = error_stack_top(error_stack);
  if (status != ERROR_STATUS_SUCCESS) {
    error_stack_print_and_reset(error_stack);
    log_fatal("load config failed with status %d: %s\n", status, cmd);
  }
  config_execute_command(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
  printf("loaded config with command: %s\n", cmd);
  printf("seed: %" PRIu64 "\n",
         thread_control_get_seed(config_get_thread_control(config)));
}

void timeout_handler(int __attribute__((unused)) signum) {
  // Long jump back to the main function if the alarm triggers
  longjmp(env, 1);
}

bool load_and_exec_config_or_die_timed(Config *config, const char *cmd,
                                       int seconds) {
  // Set up the signal handler
  sig_t prev = signal(SIGALRM, timeout_handler);
  if (prev == SIG_ERR) {
    abort();
  }
  // Set a time limit
  alarm(seconds);

  // Save the environment for long jump
  if (setjmp(env) == 0) {
    // If setjmp returns 0, continue execution

    // Call the original function
    load_and_exec_config_or_die(config, cmd);

    // Disable the alarm
    alarm(0);

    // If the function completes within the time limit, return true
    return true;
  }
  // If longjmp is called, the function exceeded the time limit
  return false;
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
  ErrorStack *error_stack = error_stack_create();
  Config *config = config_create_default_with_data_paths(
      error_stack, DEFAULT_TEST_DATA_PATH);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  load_and_exec_config_or_die(config, cmd);
  error_stack_destroy(error_stack);
  return config;
}

WMP *wmp_create_or_die(const char *data_paths, const char *wmp_name) {
  ErrorStack *error_stack = error_stack_create();
  WMP *wmp = wmp_create(data_paths, wmp_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
  return wmp;
}

KLV *klv_create_or_die(const char *data_paths, const char *klv_name) {
  ErrorStack *error_stack = error_stack_create();
  KLV *klv = klv_create(data_paths, klv_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
  return klv;
}

void klv_write_or_die(const KLV *klv, const char *data_paths,
                      const char *klv_name) {
  ErrorStack *error_stack = error_stack_create();
  klv_write(klv, data_paths, klv_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
}

KLV *klv_read_from_csv_or_die(const LetterDistribution *ld,
                              const char *data_paths, const char *leaves_name) {
  ErrorStack *error_stack = error_stack_create();
  KLV *klv = klv_read_from_csv(ld, data_paths, leaves_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
  return klv;
}

void klv_write_to_csv_or_die(KLV *klv, const LetterDistribution *ld,
                             const char *data_paths, const char *csv_name) {
  ErrorStack *error_stack = error_stack_create();
  klv_write_to_csv(klv, ld, data_paths, csv_name, NULL, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
}

char *get_string_from_file_or_die(const char *filename) {
  ErrorStack *error_stack = error_stack_create();
  char *result = get_string_from_file(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
  return result;
}

Config *config_create_default_test(void) {
  ErrorStack *error_stack = error_stack_create();
  Config *config = config_create_default_with_data_paths(
      error_stack, DEFAULT_TEST_DATA_PATH);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
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

// Empties the passed in move list and returns a SortedMoveList
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

void print_game(const Game *game, const MoveList *move_list) {
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

void print_english_rack(const Rack *rack) {
  for (int i = 0; i < rack_get_letter(rack, BLANK_MACHINE_LETTER); i++) {
    printf("?");
  }
  const uint16_t ld_size = rack_get_dist_size(rack);
  for (int i = 1; i < ld_size; i++) {
    const uint16_t num_letter = rack_get_letter(rack, i);
    for (uint16_t j = 0; j < num_letter; j++) {
      printf("%c", (char)(i + 'A' - 1));
    }
  }
}

void print_rack(const Rack *rack, const LetterDistribution *ld) {
  if (!rack) {
    printf("(null)\n");
    return;
  }
  StringBuilder *rack_sb = string_builder_create();
  string_builder_add_rack(rack_sb, rack, ld, false);
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

  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
  };

  generate_moves(&args);
  SortedMoveList *sorted_move_list = sorted_move_list_create(move_list);
  play_move(sorted_move_list->moves[n], game, NULL);
  sorted_move_list_destroy(sorted_move_list);
  move_list_destroy(move_list);
}

void load_cgp_or_die(Game *game, const char *cgp) {
  ErrorStack *error_stack = error_stack_create();
  game_load_cgp(game, cgp, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("cgp load failed with %d\n", error_stack_top(error_stack));
  }
  error_stack_destroy(error_stack);
}

void game_play_n_events_or_die(GameHistory *game_history, Game *game,
                               int event_index) {
  ErrorStack *error_stack = error_stack_create();
  game_play_n_events(game_history, game, event_index, false, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    printf("Failed to play to event index %d due to the following error:\n",
           event_index);
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to play to event index %d\n", event_index);
  }
  error_stack_destroy(error_stack);
}

void game_play_to_end_or_die(GameHistory *game_history, Game *game) {
  ErrorStack *error_stack = error_stack_create();
  game_play_to_end(game_history, game, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to play to end\n");
  }
  error_stack_destroy(error_stack);
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
    if (!str2) {
      fprintf_or_die(stderr, "strings are not equal:\n>%s<\n>%s<\n", str1,
                     "(NULL)");
    } else if (!str1) {
      fprintf_or_die(stderr, "strings are not equal:\n>%s<\n>%s<\n", "(NULL)",
                     str2);
    } else {
      fprintf_or_die(stderr, "strings are not equal:\n>%s<\n>%s<\n", str1,
                     str2);
    }
    assert(0);
  }
}

void assert_racks_equal(const LetterDistribution *ld, const Rack *r1,
                        const Rack *r2) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, r1, ld, false);
  char *r1_str = string_builder_dump(sb, NULL);
  string_builder_clear(sb);
  string_builder_add_rack(sb, r2, ld, false);
  char *r2_str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  assert_strings_equal(r1_str, r2_str);
  free(r1_str);
  free(r2_str);
}

void assert_rack_equals_string(const LetterDistribution *ld, const Rack *r1,
                               const char *r2_str) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, r1, ld, false);
  char *r1_str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  assert_strings_equal(r1_str, r2_str);
  free(r1_str);
}

void assert_bags_are_equal(const Bag *b1, const Bag *b2) {
  int b1_unseen[MAX_ALPHABET_SIZE];
  memset(b1_unseen, 0, sizeof(b1_unseen));
  bag_increment_unseen_count(b1, b1_unseen);

  int b2_unseen[MAX_ALPHABET_SIZE];
  memset(b2_unseen, 0, sizeof(b2_unseen));
  bag_increment_unseen_count(b2, b2_unseen);

  for (int i = 0; i < MAX_ALPHABET_SIZE; i++) {
    if (b1_unseen[i] != b2_unseen[i]) {
      printf("Bags to not have the same amount of letter %c: %d != %d\n",
             i + 'A' - 1, b1_unseen[i], b2_unseen[i]);
      assert(0);
    }
  }

  if (bag_get_letters(b1) != bag_get_letters(b2)) {
    printf("Bags to not have the same number of letters: %d != %d\n",
           bag_get_letters(b1), bag_get_letters(b2));
    assert(0);
  }
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
        assert(bonus_squares_are_equal(board_get_bonus_square(b1, row, col),
                                       board_get_bonus_square(b2, row, col)));
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

void assert_move(const Game *game, const MoveList *move_list,
                 const SortedMoveList *sml, int move_index,
                 const char *expected_move_string) {
  if (!sml && !move_list) {
    log_fatal("sml and move_list are both null");
    return;
  }
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  StringBuilder *move_string = string_builder_create();
  const Move *move;
  if (sml) {
    move = sml->moves[move_index];
  } else {
    move = move_list_get_move(move_list, move_index);
  }
  string_builder_add_move(move_string, board, move, ld);
  if (!strings_equal(string_builder_peek(move_string), expected_move_string)) {
    fprintf_or_die(stderr, "moves are not equal\ngot: >%s<\nexp: >%s<\n",
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

void assert_games_are_equal(const Game *g1, const Game *g2,
                            const bool check_scores) {
  assert(game_get_consecutive_scoreless_turns(g1) ==
         game_get_consecutive_scoreless_turns(g2));
  assert(game_get_game_end_reason(g1) == game_get_game_end_reason(g2));

  int g1_player_on_event_index = game_get_player_on_turn_index(g1);

  const Player *g1_player_on_turn =
      game_get_player(g1, g1_player_on_event_index);
  const Player *g1_player_not_on_turn =
      game_get_player(g1, 1 - g1_player_on_event_index);

  int g2_player_on_event_index = game_get_player_on_turn_index(g2);

  const Player *g2_player_on_turn =
      game_get_player(g2, g2_player_on_event_index);
  const Player *g2_player_not_on_turn =
      game_get_player(g2, 1 - g2_player_on_event_index);

  assert_players_are_equal(g1_player_on_turn, g2_player_on_turn, check_scores);
  assert_players_are_equal(g1_player_not_on_turn, g2_player_not_on_turn,
                           check_scores);

  Board *board1 = game_get_board(g1);
  Board *board2 = game_get_board(g2);

  const Bag *bag1 = game_get_bag(g1);
  const Bag *bag2 = game_get_bag(g2);

  assert_boards_are_equal(board1, board2);
  assert_bags_are_equal(bag1, bag2);
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

void delete_fifo(const char *fifo_name) { unlink(fifo_name); }

// Board layout test helpers

void assert_board_layout_error(const char *data_paths,
                               const char *board_layout_name,
                               error_code_t expected_status) {
  BoardLayout *bl = board_layout_create();
  ErrorStack *error_stack = error_stack_create();
  board_layout_load(bl, data_paths, board_layout_name, error_stack);
  error_code_t actual_status = error_stack_top(error_stack);
  board_layout_destroy(bl);
  if (actual_status != expected_status) {
    printf("board layout load statuses do not match: %d != %d", expected_status,
           actual_status);
  }
  assert(actual_status == expected_status);
  error_stack_destroy(error_stack);
}

BoardLayout *board_layout_create_for_test(const char *data_paths,
                                          const char *board_layout_name) {
  BoardLayout *bl = board_layout_create();
  ErrorStack *error_stack = error_stack_create();
  board_layout_load(bl, data_paths, board_layout_name, error_stack);
  error_code_t actual_status = error_stack_top(error_stack);
  if (!error_stack_is_empty(error_stack)) {
    printf("board layout load failure for %s: %d\n", board_layout_name,
           actual_status);
  }
  assert(actual_status == ERROR_STATUS_SUCCESS);
  error_stack_destroy(error_stack);
  return bl;
}

void load_game_with_test_board(Game *game, const char *data_paths,
                               const char *board_layout_name) {
  BoardLayout *bl = board_layout_create_for_test(data_paths, board_layout_name);
  board_apply_layout(bl, game_get_board(game));
  game_reset(game);
  board_layout_destroy(bl);
}

char *remove_parentheses(const char *input) {
  size_t len = strlen(input);
  char *result = (char *)malloc_or_die((len + 1) * sizeof(char));
  int j = 0;
  for (size_t i = 0; i < len; i++) {
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
                                          const int move_score,
                                          const bool play_move_on_board) {
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(1);
  const MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
  };

  rack_set_to_string(game_get_ld(game), player_rack, rack_string);

  generate_moves_for_game(&move_gen_args);
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

  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(game, 0, vm_move_string, false,
                                               true, false, error_stack);
  free(vm_move_string);
  assert(error_stack_top(error_stack) == ERROR_STATUS_SUCCESS);

  if (play_move_on_board) {
    play_move(move_list_get_move(move_list, 0), game, NULL);
  }

  validated_moves_destroy(vms);
  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
}

ValidatedMoves *validated_moves_create_and_assert_status(
    const Game *game, int player_index, const char *ucgi_moves_string,
    bool allow_phonies, bool allow_unknown_exchanges, bool allow_playthrough,
    error_code_t expected_status) {
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(
      game, player_index, ucgi_moves_string, allow_phonies,
      allow_unknown_exchanges, allow_playthrough, error_stack);
  const bool ok = error_stack_top(error_stack) == expected_status;
  if (!ok) {
    printf("validated_moves_create return unexpected status for %s: %d != %d\n",
           ucgi_moves_string, error_stack_top(error_stack), expected_status);
    error_stack_print_and_reset(error_stack);
    assert(0);
  }
  error_stack_destroy(error_stack);
  return vms;
}

error_code_t config_simulate_and_return_status(const Config *config,
                                               Rack *known_opp_rack,
                                               SimResults *sim_results) {
  ErrorStack *error_stack = error_stack_create();
  set_thread_control_status_to_start(config_get_thread_control(config));
  config_simulate(config, known_opp_rack, sim_results, error_stack);
  error_code_t status = error_stack_top(error_stack);
  if (status != ERROR_STATUS_SUCCESS) {
    printf("config simulate finished with error: %d\n", status);
    error_stack_print_and_reset(error_stack);
  }
  error_stack_destroy(error_stack);
  return status;
}

ValidatedMoves *assert_validated_move_success(Game *game, const char *cgp_str,
                                              const char *move_str,
                                              int player_index,
                                              bool allow_phonies,
                                              bool allow_playthrough) {
  load_cgp_or_die(game, cgp_str);
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, player_index, move_str, allow_phonies, true,
                             allow_playthrough, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_SUCCESS);
  error_stack_destroy(error_stack);
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

void assert_simmed_plays_stats_are_equal(const SimmedPlay *sp1,
                                         const SimmedPlay *sp2, int max_plies) {
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

void assert_simmed_plays_are_equal(const SimmedPlay *sp1, const SimmedPlay *sp2,
                                   int max_plies) {
  assert(simmed_play_get_id(sp1) == simmed_play_get_id(sp2));
  assert(simmed_play_get_is_epigon(sp1) == simmed_play_get_is_epigon(sp2));
  assert_moves_are_equal(simmed_play_get_move(sp1), simmed_play_get_move(sp2));
  assert_simmed_plays_stats_are_equal(sp1, sp2, max_plies);
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
  assert(sim_results_get_node_count(sr1) == sim_results_get_node_count(sr2));
  for (int i = 0; i < sim_results_get_number_of_plays(sr1); i++) {
    assert_simmed_plays_are_equal(sim_results_get_sorted_simmed_play(sr1, i),
                                  sim_results_get_sorted_simmed_play(sr2, i),
                                  sim_results_get_max_plies(sr1));
  }
}

// Does not check that the node names are equal
void assert_kwgs_are_equal(const KWG *kwg1, const KWG *kwg2) {
  const int num_nodes = kwg_get_number_of_nodes(kwg1);
  assert(num_nodes == kwg_get_number_of_nodes(kwg2));
  for (int i = 0; i < num_nodes; i++) {
    assert(kwg_node(kwg1, i) == kwg_node(kwg2, i));
  }
}

void assert_klvs_equal(const KLV *klv1, const KLV *klv2) {
  assert_kwgs_are_equal(klv_get_kwg(klv1), klv_get_kwg(klv2));
  const int num_nodes = kwg_get_number_of_nodes(klv_get_kwg(klv1));
  const uint32_t number_of_leaves = klv_get_number_of_leaves(klv1);
  assert(number_of_leaves == klv_get_number_of_leaves(klv2));
  for (int i = 0; i < num_nodes; i++) {
    assert(klv1->word_counts[i] == klv2->word_counts[i]);
  }
  for (uint32_t i = 0; i < number_of_leaves; i++) {
    assert(within_epsilon(klv_get_indexed_leave_value(klv1, i),
                          klv_get_indexed_leave_value(klv2, i)));
  }
}

void assert_word_count(const LetterDistribution *ld,
                       const DictionaryWordList *words,
                       const char *human_readable_word, int expected_count) {
  size_t expected_length = string_length(human_readable_word);
  MachineLetter expected[BOARD_DIM];
  ld_str_to_mls(ld, human_readable_word, false, expected, expected_length);
  int count = 0;
  const int word_count = dictionary_word_list_get_count(words);
  for (int i = 0; i < word_count; i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if ((dictionary_word_get_length(word) == (uint8_t)expected_length) &&
        (memcmp(dictionary_word_get_word(word), expected, expected_length) ==
         0)) {
      count++;
    }
  }
  assert(count == expected_count);
}

BitRack string_to_bit_rack(const LetterDistribution *ld,
                           const char *rack_string) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_string);
  BitRack bit_rack = bit_rack_create_from_rack(ld, rack);
  rack_destroy(rack);
  return bit_rack;
}

// This only works on ASCII languages, e.g. English, French. Polish would need
// to support multibyte user-visible characters, but Polish isn't even supported
// for BitRack (and therefore for WMP) because the lexicon is >32 letters
// (including the blank).
void assert_word_in_buffer(const MachineLetter *buffer,
                           const char *expected_word,
                           const LetterDistribution *ld, const int word_idx,
                           const int length) {
  const int start = word_idx * length;
  char hl[2] = {0, 0};
  for (int i = 0; i < length; i++) {
    hl[0] = expected_word[i];
    assert(buffer[start + i] == ld_hl_to_ml(ld, hl));
  }
}

void assert_move_score(const Move *move, int expected_score) {
  const Equity expected_score_eq = int_to_equity(expected_score);
  assert(move_get_score(move) == expected_score_eq);
}

void assert_move_equity_int(const Move *move, int expected_equity) {
  assert_move_equity_exact(move, int_to_equity(expected_equity));
}

void assert_move_equity_exact(const Move *move, Equity expected_equity) {
  assert(move_get_equity(move) == expected_equity);
}

void assert_rack_score(const LetterDistribution *ld, const Rack *rack,
                       int expected_score) {
  assert(rack_get_score(ld, rack) == int_to_equity(expected_score));
}

void assert_validated_moves_challenge_points(const ValidatedMoves *vms, int i,
                                             int expected_challenge_points) {
  const Equity expected_challenge_points_eq =
      int_to_equity(expected_challenge_points);
  assert(validated_moves_get_challenge_points(vms, i) ==
         expected_challenge_points_eq);
}

void assert_anchor_equity_int(const AnchorHeap *ah, int i, int expected) {
  assert_anchor_equity_exact(ah, i, int_to_equity(expected));
}

void assert_anchor_equity_exact(const AnchorHeap *ah, int i, Equity expected) {
  const Equity actual = ah->anchors[i].highest_possible_equity;
  assert(actual == expected);
}

void generate_anchors_for_test(Game *game) {
  const Player *player_on_turn =
      game_get_player(game, game_get_player_on_turn_index(game));
  // We don't care about them, but exchanges will be recorded while
  // looking up leave values and it is not adding a parameter to prevent this.
  MoveList *move_list = move_list_create(1000);
  MoveGen *gen = get_movegen(/*thread_index=*/0);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = player_get_move_record_type(player_on_turn),
      .move_sort_type = player_get_move_sort_type(player_on_turn),
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
  };
  gen_load_position(gen, &args);
  gen_look_up_leaves_and_record_exchanges(gen);
  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    wmp_move_gen_check_nonplaythrough_existence(
        &gen->wmp_move_gen, gen->number_of_tiles_in_bag > 0, &gen->leave_map);
  }
  gen_shadow(gen);
  move_list_destroy(move_list);
}

void extract_sorted_anchors_for_test(AnchorHeap *sorted_anchors) {
  MoveGen *gen = get_movegen(/*thread_index=*/0);
  anchor_heap_reset(sorted_anchors);
  while (gen->anchor_heap.count > 0) {
    sorted_anchors->anchors[sorted_anchors->count++] =
        anchor_heap_extract_max(&gen->anchor_heap);
  }
}

void set_klv_leave_value(const KLV *klv, const LetterDistribution *ld,
                         const char *rack_str, const Equity equity) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_str);
  const uint32_t klv_word_index = klv_get_word_index(klv, rack);
  klv_set_indexed_leave_value(klv, klv_word_index, equity);
  rack_destroy(rack);
}

error_code_t test_parse_gcg(const char *gcg_filename, Config *config,
                            GameHistory *game_history) {
  ErrorStack *error_stack = error_stack_create();
  char *gcg_filepath = data_filepaths_get_readable_filename(
      config_get_data_paths(config), gcg_filename, DATA_FILEPATH_TYPE_GCG,
      error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to get gcg filepath for test: %s\n", gcg_filename);
  }
  config_parse_gcg(config, gcg_filepath, game_history, error_stack);
  error_code_t gcg_parse_status = error_stack_top(error_stack);
  error_stack_print_and_reset(error_stack);
  error_stack_destroy(error_stack);
  free(gcg_filepath);
  return gcg_parse_status;
}

error_code_t test_parse_gcg_string(const char *gcg_string, Config *config,
                                   GameHistory *game_history) {
  ErrorStack *error_stack = error_stack_create();
  config_parse_gcg_string(config, gcg_string, game_history, error_stack);
  error_code_t gcg_parse_status = error_stack_top(error_stack);
  error_stack_print_and_reset(error_stack);
  error_stack_destroy(error_stack);
  return gcg_parse_status;
}

error_code_t test_parse_gcg_file(const char *gcg_filename, Config *config,
                                 GameHistory *game_history) {
  ErrorStack *error_stack = error_stack_create();
  config_parse_gcg(config, gcg_filename, game_history, error_stack);
  error_code_t gcg_parse_status = error_stack_top(error_stack);
  error_stack_print_and_reset(error_stack);
  error_stack_destroy(error_stack);
  return gcg_parse_status;
}

// Resets the history before loading the GCG
void load_game_history_with_gcg_string(Config *config, const char *gcg_header,
                                       const char *gcg_content) {
  GameHistory *game_history = config_get_game_history(config);
  game_history_reset(game_history);
  char *gcg_string = get_formatted_string("%s%s", gcg_header, gcg_content);
  assert(test_parse_gcg_string(gcg_string, config, game_history) ==
         ERROR_STATUS_SUCCESS);
  free(gcg_string);
}

// Resets the history before loading the GCG
void load_game_history_with_gcg(Config *config, const char *gcg_file) {
  ErrorStack *error_stack = error_stack_create();
  char *gcg_filename = data_filepaths_get_readable_filename(
      config_get_data_paths(config), gcg_file, DATA_FILEPATH_TYPE_GCG,
      error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to get gcg filepath for test: %s\n", gcg_filename);
  }
  error_stack_destroy(error_stack);
  GameHistory *game_history = config_get_game_history(config);
  game_history_reset(game_history);
  assert(test_parse_gcg_file(gcg_filename, config, game_history) ==
         ERROR_STATUS_SUCCESS);
  free(gcg_filename);
}

void assert_config_exec_status(Config *config, const char *cmd,
                               error_code_t expected_error_code) {
  ErrorStack *error_stack = error_stack_create();
  set_thread_control_status_to_start(config_get_thread_control(config));
  config_load_command(config, cmd, error_stack);
  error_code_t load_status = error_stack_top(error_stack);

  // If we expect an error and got it during load, that's the expected result
  if (load_status != ERROR_STATUS_SUCCESS) {
    if (load_status != expected_error_code) {
      printf("config load error types do not match:\nexpected: %d\nactual: "
             "%d\n>%s<\n",
             expected_error_code, load_status, cmd);
      error_stack_print_and_reset(error_stack);
      abort();
    }
    error_stack_destroy(error_stack);
    return;
  }

  config_execute_command(config, error_stack);
  error_code_t actual_error_code = error_stack_top(error_stack);
  if (actual_error_code != expected_error_code) {
    printf("config exec error types do not match:\nexpected: %d\nactual: "
           "%d\n>%s<\n",
           expected_error_code, actual_error_code, cmd);
    error_stack_print_and_reset(error_stack);
    abort();
  }
  error_stack_destroy(error_stack);
}