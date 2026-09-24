#include "position_lengths_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/move.h"
#include "../src/ent/move_undo.h"
#include "../src/ent/players_data.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmp_maker.h"
#include "../src/impl/word_info_table_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { POSITION_ORACLE_ROOT = 283000003, POSITION_QUERY_COUNT = 2000 };

static void add_literal(DictionaryWordList *words, const char *literal) {
  MachineLetter letters[BOARD_DIM] = {0};
  const size_t length = strlen(literal);
  assert(length > 0 && length <= BOARD_DIM);
  for (size_t position = 0; position < length; position++) {
    assert(literal[position] >= 'A' && literal[position] <= 'Z');
    letters[position] = (MachineLetter)(literal[position] - 'A' + 1);
  }
  dictionary_word_list_add_word(words, letters, (int)length);
}

static DictionaryWordList *oracle_words(void) {
  const char *const literals[] = {
      "A",       "AA",      "AB",        "AT",     "BA",     "ZZ",
      "AAA",     "ABA",     "AAAA",      "ABAB",   "CATS",   "TATA",
      "ABABA",   "AAAAA",   "TATAT",     "ABABAB", "SCATS",  "ABCDE",
      "XABCDEY", "ABCDEFG", "XABCDEFGY", "BE",     "BETTER", "BETTERS"};
  DictionaryWordList *words = dictionary_word_list_create();
  for (size_t index = 0; index < sizeof(literals) / sizeof(literals[0]);
       index++) {
    add_literal(words, literals[index]);
  }
  MachineLetter longest[BOARD_DIM];
  for (int position = 0; position < BOARD_DIM; position++) {
    longest[position] = (MachineLetter)(position + 1);
  }
  dictionary_word_list_add_word(words, longest, BOARD_DIM);
  longest[0] = 1;
  longest[1] = 20;
  longest[BOARD_DIM - 2] = 1;
  longest[BOARD_DIM - 1] = 20;
  dictionary_word_list_add_word(words, longest, BOARD_DIM);
  dictionary_word_list_sort(words);
  return words;
}

// Enumerate literal occurrences directly. This oracle does not use trie
// traversal, the derived cache, residual counts, or a transposed plane.
static uint32_t literal_lengths(const DictionaryWordList *words,
                                const DictionaryWord *base, int position) {
  uint32_t expected = 0;
  const int base_length = dictionary_word_get_length(base);
  const MachineLetter *base_letters = dictionary_word_get_word(base);
  for (int index = 0; index < dictionary_word_list_get_count(words); index++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, index);
    const int length = dictionary_word_get_length(word);
    if (position >= 0 && position + base_length <= length &&
        memcmp(dictionary_word_get_word(word) + position, base_letters,
               (size_t)base_length) == 0) {
      expected |= UINT32_C(1) << length;
    }
  }
  return expected;
}

static void assert_literal_cache(const DictionaryWordList *words,
                                 const WordInfoTable *wit) {
  assert(wit->kwg_hash != 0);
  assert(wit->position_lengths[0] == NULL);
  assert(wit->position_lengths[1] == NULL);
  for (int index = 0; index < dictionary_word_list_get_count(words); index++) {
    const DictionaryWord *base = dictionary_word_list_get_word(words, index);
    const int length = dictionary_word_get_length(base);
    const uint32_t *ordinary =
        word_info_table_lookup(wit, dictionary_word_get_word(base), length);
    assert(ordinary != NULL);
    const uint32_t *positions =
        word_info_table_get_position_lengths(wit, ordinary, length);
    if (length == 1) {
      assert(positions == NULL);
      continue;
    }
    assert(positions != NULL);
    for (int position = 0; position <= BOARD_DIM - length; position++) {
      assert(positions[position] == literal_lengths(words, base, position));
    }
    // The base itself must exist even though there are no outside letters.
    assert((positions[0] & (UINT32_C(1) << length)) != 0);
  }
}

static void test_literal_derivation(void) {
  DictionaryWordList *words = oracle_words();
  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);
  WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
  assert_literal_cache(words, wit);
  WordInfoTable *unidentified = make_word_info_table_from_words(words);
  assert(unidentified->kwg_hash == 0);
  word_info_table_build_position_lengths(unidentified);
  for (int length = 0; length <= BOARD_DIM; length++) {
    assert(unidentified->position_lengths[length] == NULL);
  }
  word_info_table_destroy(unidentified);
  // Explicit reconstruction must be idempotent and replace stale contents.
  wit->position_lengths[2][0] = 0;
  word_info_table_build_position_lengths(wit);
  assert_literal_cache(words, wit);
  // Terminal IDs need not follow lexical order. Reversing IDs must simply
  // permute cache rows, without changing the literal result for any base.
  WitTrie *permuted = &wit->tries[2];
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t node = 0; node < permuted->num_nodes; node++) {
      if (permuted->node_value[node] >= 0) {
        permuted->node_value[node] =
            (int32_t)permuted->num_values - 1 - permuted->node_value[node];
      }
    }
    word_info_table_build_position_lengths(wit);
    assert_literal_cache(words, wit);
  }
  const MachineLetter zz[] = {26, 26};
  const uint32_t *empty_positions = word_info_table_get_position_lengths(
      wit, word_info_table_lookup(wit, zz, 2), 2);
  assert(empty_positions != NULL && empty_positions[1] == 0);
  const MachineLetter unknown[] = {26, 24};
  assert(word_info_table_lookup(wit, unknown, 2) == NULL);
  const MachineLetter at[] = {1, 20};
  const uint32_t *ordinary = word_info_table_lookup(wit, at, 2);
  const uint32_t *positions =
      word_info_table_get_position_lengths(wit, ordinary, 2);
  assert(positions != NULL);
  // CAT is absent, but CATS exists. An exact miss cannot stop extension.
  assert((positions[1] & (UINT32_C(1) << 3)) == 0);
  assert((positions[1] & (UINT32_C(1) << 4)) != 0);
  assert((positions[BOARD_DIM - 2] & (UINT32_C(1) << BOARD_DIM)) != 0);
  WordInfoTable *foreign = make_word_info_table_from_kwg(kwg);
  const uint32_t *foreign_row = word_info_table_lookup(foreign, at, 2);
  assert(word_info_table_get_position_lengths(wit, foreign_row, 2) == NULL);
  assert(word_info_table_get_position_lengths(wit, ordinary + 1, 2) == NULL);
  assert(word_info_table_get_position_lengths(wit, NULL, 2) == NULL);
  assert(word_info_table_get_position_lengths(wit, ordinary, 1) == NULL);
  assert(word_info_table_get_position_lengths(wit, ordinary, BOARD_DIM + 1) ==
         NULL);
  const WitTrie *trie = &wit->tries[2];
  const uint32_t *past_end =
      trie->values + ((size_t)trie->num_values * (size_t)wit_stride_for_len(2));
  assert(word_info_table_get_position_lengths(wit, past_end, 2) == NULL);
  word_info_table_clear_position_lengths(foreign);
  assert(word_info_table_get_position_lengths(foreign, foreign_row, 2) == NULL);
  foreign->kwg_hash = 0;
  word_info_table_build_position_lengths(foreign);
  for (int length = 0; length <= BOARD_DIM; length++) {
    assert(foreign->position_lengths[length] == NULL);
  }

  // Probe length gaps and suffix boundaries independently at varied positions.
  XoshiroPRNG *prng = prng_create(POSITION_ORACLE_ROOT);
  const int word_count = dictionary_word_list_get_count(words);
  int *eligible = malloc_or_die((size_t)word_count * sizeof(int));
  int eligible_count = 0;
  for (int index = 0; index < word_count; index++) {
    if (dictionary_word_get_length(
            dictionary_word_list_get_word(words, index)) >= 2) {
      eligible[eligible_count++] = index;
    }
  }
  assert(eligible_count > 0);
  for (int query = 0; query < POSITION_QUERY_COUNT; query++) {
    const int index = eligible[prng_next(prng) % (uint64_t)eligible_count];
    const DictionaryWord *base = dictionary_word_list_get_word(words, index);
    const int length = dictionary_word_get_length(base);
    const int position =
        (int)(prng_next(prng) % (uint64_t)(BOARD_DIM - length + 1));
    const int minimum = (int)(prng_next(prng) % (BOARD_DIM + 2));
    const uint32_t *row =
        word_info_table_lookup(wit, dictionary_word_get_word(base), length);
    const uint32_t *masks =
        word_info_table_get_position_lengths(wit, row, length);
    assert(masks != NULL);
    bool literal_suffix = false;
    const uint32_t expected = literal_lengths(words, base, position);
    for (int final_length = minimum; final_length <= BOARD_DIM;
         final_length++) {
      literal_suffix |= (expected & (UINT32_C(1) << final_length)) != 0;
    }
    assert((masks[position] >> minimum != 0) == literal_suffix);
  }
  free(eligible);
  prng_destroy(prng);
  word_info_table_destroy(foreign);
  word_info_table_destroy(wit);
  kwg_destroy(kwg);
  dictionary_word_list_destroy(words);
}

static void test_load_rebuild_and_cleanup(void) {
  DictionaryWordList *words = oracle_words();
  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
  WordInfoTable *loaded = calloc_or_die(1, sizeof(WordInfoTable));
  ErrorStack *errors = error_stack_create();
  char *filename = data_filepaths_get_writable_filename(
      DEFAULT_TEST_DATA_PATH, "position_lengths_roundtrip",
      DATA_FILEPATH_TYPE_WORD_INFO_TABLE, errors);
  assert(error_stack_is_empty(errors));
  word_info_table_write_to_file(wit, filename, errors);
  assert(error_stack_is_empty(errors));
  word_info_table_load(loaded, "position_lengths_roundtrip", filename, errors);
  assert(error_stack_is_empty(errors));
  assert_literal_cache(words, loaded);
  // Version3 ordinary rows remain loadable and still derive literal positions.
  FILE *file = fopen_or_die(filename, "r+b");
  const int written = fputc(3, file);
  assert(written == 3);
  fclose_or_die(file);
  word_info_table_load(loaded, "position_lengths_v3", filename, errors);
  assert(error_stack_is_empty(errors));
  assert_literal_cache(words, loaded);
  file = fopen_or_die(filename, "wb");
  const int truncated = fputc(4, file);
  assert(truncated == 4);
  fclose_or_die(file);
  word_info_table_load(loaded, "position_lengths_truncated", filename, errors);
  assert(!error_stack_is_empty(errors));
  for (int length = 0; length <= BOARD_DIM; length++) {
    assert(loaded->position_lengths[length] == NULL);
  }
  error_stack_reset(errors);
  word_info_table_write_to_file(wit, filename, errors);
  word_info_table_load(loaded, "position_lengths_restored", filename, errors);
  assert(error_stack_is_empty(errors));
  assert_literal_cache(words, loaded);
  remove_or_die(filename);
  free(filename);
  error_stack_destroy(errors);
  word_info_table_destroy(loaded);
  word_info_table_destroy(wit);
  kwg_destroy(kwg);
  dictionary_word_list_destroy(words);
}

static void replace_shared_data(PlayersData *players, players_data_t type,
                                void *data) {
  assert(players_data_get_is_shared(players, type));
  void *original = players_data_get_data(players, type, 0);
  assert(original == players_data_get_data(players, type, 1));
  switch (type) {
  case PLAYERS_DATA_TYPE_KWG:
    kwg_destroy(original);
    break;
  case PLAYERS_DATA_TYPE_WMP:
    wmp_destroy(original);
    break;
  case PLAYERS_DATA_TYPE_WIT:
    word_info_table_destroy(original);
    break;
  default:
    log_fatal("unexpected tiny fixture data type %d", type);
    break;
  }
  players_data_set_data(players, type, 0, data);
  players_data_set_data(players, type, 1, data);
}

static Config *tiny_config(bool with_wmp) {
  // Keep two blanks on either board size; the WMP format supports at most two.
  Config *config =
      config_create_or_die("set -lex CSW21 -ld english -wmp false -rit false "
                           "-wit false -s1 score -s2 score "
                           "-r1 all -r2 all -numplays 100000");
  const char *const literals[] = {"AT",    "CATS",    "SCATS",
                                  "BE",    "BETTER",  "BETTERS",
                                  "ABCDE", "XABCDEY", "CATXBEY"};
  DictionaryWordList *words = dictionary_word_list_create();
  for (size_t index = 0; index < sizeof(literals) / sizeof(literals[0]);
       index++) {
    add_literal(words, literals[index]);
  }
  dictionary_word_list_sort(words);
  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);
  PlayersData *players = config_get_players_data(config);
  replace_shared_data(players, PLAYERS_DATA_TYPE_KWG, kwg);
  if (with_wmp) {
    WMP *wmp = make_wmp_from_words(words, config_get_ld(config), 1);
    replace_shared_data(players, PLAYERS_DATA_TYPE_WMP, wmp);
    replace_shared_data(players, PLAYERS_DATA_TYPE_WIT,
                        make_word_info_table_from_kwg(kwg));
  }
  dictionary_word_list_destroy(words);
  return config;
}

static char *position_string(const char *line, int first_col, const char *rack,
                             bool vertical) {
  char board[BOARD_DIM][BOARD_DIM];
  memset(board, '.', sizeof(board));
  for (size_t index = 0; index < strlen(line); index++) {
    const int col = first_col + (int)index;
    assert(col < BOARD_DIM);
    board[vertical ? col : BOARD_DIM / 2][vertical ? BOARD_DIM / 2 : col] =
        line[index];
  }
  StringBuilder *builder = string_builder_create();
  for (int row = 0; row < BOARD_DIM; row++) {
    int empty = 0;
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board[row][col] == '.') {
        empty++;
      } else {
        if (empty != 0) {
          string_builder_add_int(builder, empty);
          empty = 0;
        }
        string_builder_add_char(builder, board[row][col]);
      }
    }
    if (empty != 0) {
      string_builder_add_int(builder, empty);
    }
    string_builder_add_char(builder, row == BOARD_DIM - 1 ? ' ' : '/');
  }
  string_builder_add_formatted_string(builder, "%s/ 0/0 0", rack);
  char *result = string_builder_dump(builder, NULL);
  string_builder_destroy(builder);
  return result;
}

static MoveList *all_moves(const Game *game) {
  MoveList *moves = move_list_create(10000);
  const MoveGenArgs args = {
      .game = game,
      .move_list = moves,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves_for_game(&args);
  assert(move_list_get_count(moves) < 10000);
  return moves;
}

static void assert_same_moves(const Game *game, const Game *reference) {
  MoveList *actual = all_moves(game);
  MoveList *expected = all_moves(reference);
  SortedMoveList *sorted = sorted_move_list_create(actual);
  SortedMoveList *oracle = sorted_move_list_create(expected);
  assert(sorted->count == oracle->count);
  for (int index = 0; index < sorted->count; index++) {
    assert(compare_moves(sorted->moves[index], oracle->moves[index], true) ==
           -1);
  }
  sorted_move_list_destroy(sorted);
  sorted_move_list_destroy(oracle);
  move_list_destroy(actual);
  move_list_destroy(expected);
}

static void assert_word_move(const Game *game, const char *word, int start,
                             bool vertical) {
  char command[64];
  if (vertical) {
    (void)snprintf(command, sizeof(command), "%c%d %s", 'A' + (BOARD_DIM / 2),
                   start + 1, word);
  } else {
    (void)snprintf(command, sizeof(command), "%d%c %s", (BOARD_DIM / 2) + 1,
                   'A' + start, word);
  }
  ValidatedMoves *validated = validated_moves_create_and_assert_status(
      game, game_get_player_on_turn_index(game), command, false, false,
      ERROR_STATUS_SUCCESS);
  MoveList *moves = all_moves(game);
  bool found = false;
  for (int index = 0; index < move_list_get_count(moves); index++) {
    found |= compare_moves_without_equity(
                 move_list_get_move(moves, index),
                 validated_moves_get_move(validated, 0), true) == -1;
  }
  assert(found);
  move_list_destroy(moves);
  validated_moves_destroy(validated);
}

static void test_shadow_boundary_moves(void) {
  Config *config = tiny_config(true);
  Config *reference_config = tiny_config(false);
  Game *game = config_game_create(config);
  Game *reference = config_game_create(reference_config);
  Game *reused = game_duplicate(game);
  WordInfoTable *wit =
      players_data_get_word_info_table(config_get_players_data(config), 0);
  const struct {
    const char *line;
    const char *rack;
    const char *required;
    int relative_start;
  } cases[] = {
      // A one-tile base and a phony complete block have no indexed base row.
      // Both must still be extendable into the legal word CATS.
      {"A", "CTS", "CATS", -1},       {"CA", "TS", "CATS", 0},
      {"AT", "CS", "CATS", -1},       {"AT", "CSS", "SCATS", -2},
      {"BE", "TTER", "BETTER", 0},    {"AT", "C?", NULL, 0},
      {"AT", "??", NULL, 0},          {"aT", "CS", NULL, 0},
      {"ABCDE", "XY", "XABCDEY", -1}, {"AT.BE", "CXY", "CATXBEY", -1},
      {"SCATS", "BE", NULL, 0},       {"", "CS", NULL, 0},
  };
  for (int vertical = BOARD_HORIZONTAL_DIRECTION;
       vertical <= BOARD_VERTICAL_DIRECTION; vertical++) {
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
      const int first_col = (BOARD_DIM / 2) - 2;
      char *cgp = position_string(cases[index].line, first_col,
                                  cases[index].rack, vertical != 0);
      load_cgp_or_die(game, cgp);
      load_cgp_or_die(reference, cgp);
      assert_same_moves(game, reference);
      if (cases[index].required != NULL) {
        assert_word_move(game, cases[index].required,
                         first_col + cases[index].relative_start,
                         vertical != 0);
      }
      game_copy(reused, game);
      assert_same_moves(reused, reference);
      // Reusing a loaded board with the derived cache missing must fall back.
      word_info_table_clear_position_lengths(wit);
      assert_same_moves(game, reference);
      word_info_table_build_position_lengths(wit);
      assert_same_moves(reused, reference);
      free(cgp);
    }
    char *edge = position_string("AT", BOARD_DIM - 3, "CS", vertical != 0);
    load_cgp_or_die(game, edge);
    load_cgp_or_die(reference, edge);
    assert_same_moves(game, reference);
    assert_word_move(game, "CATS", BOARD_DIM - 4, vertical != 0);
    free(edge);
  }
  game_destroy(reused);
  game_destroy(reference);
  game_destroy(game);
  config_destroy(reference_config);
  config_destroy(config);
}

static void test_shadow_foreign_rows_and_undo(void) {
  Config *config = tiny_config(true);
  Config *reference_config = tiny_config(false);
  Game *game = config_game_create(config);
  Game *reference = config_game_create(reference_config);
  const int first_col = (BOARD_DIM / 2) - 1;
  char *cgp = position_string("AT", first_col, "CS", false);
  load_cgp_or_die(game, cgp);
  load_cgp_or_die(reference, cgp);
  const PlayersData *players = config_get_players_data(config);
  WordInfoTable *foreign =
      make_word_info_table_from_kwg(players_data_get_kwg(players, 0));
  const MachineLetter at[] = {1, 20};
  const uint32_t *foreign_row = word_info_table_lookup(foreign, at, 2);
  assert(foreign_row != NULL);
  for (int cross_index = 0; cross_index < 2; cross_index++) {
    board_set_wit_block(game_get_board(game), BOARD_DIM / 2, first_col, 0,
                        cross_index, foreign_row, 2);
  }
  assert_same_moves(game, reference);
  assert_word_move(game, "CATS", first_col - 1, false);
  // A cache with all borrowed rows absent must likewise remain permissive.
  board_clear_wit_cache(game_get_board(game));
  assert_same_moves(game, reference);
  game_gen_all_cross_sets(game);
  word_info_table_destroy(foreign);

  for (int incremental = 0; incremental < 2; incremental++) {
    load_cgp_or_die(game, cgp);
    load_cgp_or_die(reference, cgp);
    game_seed(game, POSITION_ORACLE_ROOT);
    game_seed(reference, POSITION_ORACLE_ROOT);
    if (incremental) {
      while (bag_get_letters(game_get_bag(game)) > 0) {
        bag_draw_random_letter(game_get_bag(game), 0);
        bag_draw_random_letter(game_get_bag(reference), 0);
      }
    } else {
      game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
      game_set_backup_mode(reference, BACKUP_MODE_SIMULATION);
    }
    char command[64];
    (void)snprintf(command, sizeof(command), "%d%c CATS", (BOARD_DIM / 2) + 1,
                   'A' + first_col - 1);
    ValidatedMoves *validated = validated_moves_create_and_assert_status(
        game, 0, command, false, false, ERROR_STATUS_SUCCESS);
    const Move *move = validated_moves_get_move(validated, 0);
    if (incremental) {
      MoveUndo undo;
      MoveUndo reference_undo;
      play_move_incremental(move, game, &undo);
      play_move_incremental(move, reference, &reference_undo);
      update_cross_set_for_move_from_undo(&undo, game);
      update_cross_set_for_move_from_undo(&reference_undo, reference);
      assert_same_moves(game, reference);
      unplay_move_incremental(game, &undo);
      unplay_move_incremental(reference, &reference_undo);
    } else {
      play_move(move, game, NULL);
      play_move(move, reference, NULL);
      assert_same_moves(game, reference);
      game_unplay_last_move(game);
      game_unplay_last_move(reference);
    }
    assert_same_moves(game, reference);
    assert_word_move(game, "CATS", first_col - 1, false);
    validated_moves_destroy(validated);
  }
  free(cgp);
  game_destroy(reference);
  game_destroy(game);
  config_destroy(reference_config);
  config_destroy(config);
}

void test_position_lengths(void) {
  test_literal_derivation();
  test_load_rebuild_and_cleanup();
  test_shadow_boundary_moves();
  test_shadow_foreign_rows_and_undo();
  printf("POSITION_LENGTHS_TEST_PASS board=%d root=%d queries=%d\n", BOARD_DIM,
         POSITION_ORACLE_ROOT, POSITION_QUERY_COUNT);
}

void test_position_lengths_loaded(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -rit true -ritmmap true -wit true");
  const WordInfoTable *wit =
      players_data_get_word_info_table(config_get_players_data(config), 0);
  assert(wit != NULL && wit->kwg_hash != 0);
  assert(wit->position_lengths[1] == NULL);
  size_t bases = 0;
  size_t entries = 0;
  uint64_t digest = UINT64_C(14695981039346656037);
  for (int length = 2; length <= BOARD_DIM; length++) {
    const WitTrie *trie = &wit->tries[length];
    const size_t stride = (size_t)wit_stride_for_len(length);
    assert(trie->num_values == 0 || wit->position_lengths[length] != NULL);
    bases += trie->num_values;
    entries += (size_t)trie->num_values * stride;
    for (size_t base = 0; base < trie->num_values; base++) {
      const uint32_t *row = word_info_table_get_position_lengths(
          wit, trie->values + (base * stride), length);
      assert(row != NULL && (row[0] & (UINT32_C(1) << length)) != 0);
      for (size_t position = 0; position < stride; position++) {
        digest = (digest ^ row[position]) * UINT64_C(1099511628211);
      }
    }
  }
  const MachineLetter at[] = {1, 20};
  const uint32_t *row = word_info_table_get_position_lengths(
      wit, word_info_table_lookup(wit, at, 2), 2);
  assert(row != NULL && (row[1] & (UINT32_C(1) << 3)) != 0);
  assert(bases > 0 && entries > bases);
  printf(
      "POSITION_LENGTHS_LOADED kwg=%llu bases=%llu entries=%llu digest=%llu\n",
      (unsigned long long)wit->kwg_hash, (unsigned long long)bases,
      (unsigned long long)entries, (unsigned long long)digest);
  config_destroy(config);
}
