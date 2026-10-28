#include "woadwage_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/kwg_woadwage.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Woadwage on a lexicon plays exactly as the classic game does on the
// lexicon's words and every two of them run together. These tests build both
// lexica from a sample of CSW21 and check woadwage word validity, cross sets
// and move generation against classic generation on the pairs.

enum {
  // The sample: every two-letter word of CSW21, and every
  // WOADWAGE_TEST_THREE_LETTER_STRIDE-th three-letter word and
  // WOADWAGE_TEST_LONG_WORD_STRIDE-th longer word.
  WOADWAGE_TEST_THREE_LETTER_STRIDE = 12,
  WOADWAGE_TEST_LONG_WORD_STRIDE = 3000,
  WOADWAGE_TEST_RANDOM_STRINGS = 20000,
  WOADWAGE_TEST_RANDOM_CROSS_SETS = 20000,
  WOADWAGE_TEST_GAMES = 8,
  WOADWAGE_TEST_MAX_TURNS = 40,
  WOADWAGE_TEST_MOVE_CAPACITY = 1 << 15,
  // The games whose first position with an empty bag is solved: all but 2, 6
  // and 7, whose solves take one to four seconds each.
  WOADWAGE_TEST_ENDGAME_GAMES =
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5),
};

static const char *const WOADWAGE_TEST_WORDS_LEXICON = "CSW21_woadwage_words";
static const char *const WOADWAGE_TEST_PAIRS_LEXICON = "CSW21_woadwage_pairs";

static uint64_t woadwage_test_random(uint64_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

static void woadwage_test_write_kwg(const DictionaryWordList *words,
                                    const char *name) {
  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);
  ErrorStack *error_stack = error_stack_create();
  char *path = data_filepaths_get_writable_filename(
      DEFAULT_TEST_DATA_PATH, name, DATA_FILEPATH_TYPE_KWG, error_stack);
  assert(error_stack_is_empty(error_stack));
  kwg_write_to_file(kwg, path, error_stack);
  assert(error_stack_is_empty(error_stack));
  free(path);
  kwg_destroy(kwg);
  error_stack_destroy(error_stack);
}

static void woadwage_test_remove_kwg(const char *name) {
  ErrorStack *error_stack = error_stack_create();
  char *path = data_filepaths_get_writable_filename(
      DEFAULT_TEST_DATA_PATH, name, DATA_FILEPATH_TYPE_KWG, error_stack);
  assert(error_stack_is_empty(error_stack));
  remove_or_die(path);
  free(path);
  error_stack_destroy(error_stack);
}

// Fills words with the sample of CSW21 and pairs with the sample's words and
// every two of them that fit on the board run together, and writes both
// lexica.
static void woadwage_test_create_lexica(DictionaryWordList *words,
                                        DictionaryWordList *pairs) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  const KWG *csw = players_data_get_kwg(config_get_players_data(config), 0);
  DictionaryWordList *all_words = dictionary_word_list_create();
  kwg_write_words(csw, kwg_get_dawg_root_node_index(csw), all_words, NULL);
  int three_letter_words_seen = 0;
  int long_words_seen = 0;
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(all_words);
       word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(all_words, word_idx);
    const int length = dictionary_word_get_length(word);
    bool sampled = length == 2;
    if (length == 3) {
      sampled =
          three_letter_words_seen++ % WOADWAGE_TEST_THREE_LETTER_STRIDE == 0;
    } else if (length > 3) {
      sampled = long_words_seen++ % WOADWAGE_TEST_LONG_WORD_STRIDE == 0;
    }
    if (sampled) {
      dictionary_word_list_add_word(words, dictionary_word_get_word(word),
                                    length);
    }
  }
  dictionary_word_list_destroy(all_words);
  config_destroy(config);

  DictionaryWordList *unsorted_pairs = dictionary_word_list_create();
  const int word_count = dictionary_word_list_get_count(words);
  for (int first_idx = 0; first_idx < word_count; first_idx++) {
    const DictionaryWord *first =
        dictionary_word_list_get_word(words, first_idx);
    const int first_length = dictionary_word_get_length(first);
    dictionary_word_list_add_word(
        unsorted_pairs, dictionary_word_get_word(first), first_length);
    for (int second_idx = 0; second_idx < word_count; second_idx++) {
      const DictionaryWord *second =
          dictionary_word_list_get_word(words, second_idx);
      const int second_length = dictionary_word_get_length(second);
      if (first_length + second_length > BOARD_DIM) {
        continue;
      }
      MachineLetter pair[BOARD_DIM];
      for (int letter_idx = 0; letter_idx < first_length; letter_idx++) {
        pair[letter_idx] = dictionary_word_get_word(first)[letter_idx];
      }
      for (int letter_idx = 0; letter_idx < second_length; letter_idx++) {
        pair[first_length + letter_idx] =
            dictionary_word_get_word(second)[letter_idx];
      }
      dictionary_word_list_add_word(unsorted_pairs, pair,
                                    first_length + second_length);
    }
  }
  dictionary_word_list_sort(unsorted_pairs);
  dictionary_word_list_unique(unsorted_pairs, pairs);
  dictionary_word_list_destroy(unsorted_pairs);

  woadwage_test_write_kwg(words, WOADWAGE_TEST_WORDS_LEXICON);
  woadwage_test_write_kwg(pairs, WOADWAGE_TEST_PAIRS_LEXICON);
}

// Whether letters[0..length) is a word of the KWG's lexicon.
static bool woadwage_test_dawg_contains(const KWG *kwg,
                                        const MachineLetter *letters,
                                        int length) {
  uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
  bool accepts = false;
  for (int letter_idx = 0; letter_idx < length; letter_idx++) {
    if (node_index == 0) {
      return false;
    }
    const uint64_t list_tiles = kwg_get_sibling_tiles(kwg)[node_index];
    const uint64_t tile_bit = (uint64_t)1 << letters[letter_idx];
    if ((list_tiles & tile_bit) == 0) {
      return false;
    }
    const uint32_t node = kwg_list_node(kwg, node_index, list_tiles, tile_bit);
    node_index = kwg_node_arc_index(node);
    accepts = kwg_node_accepts(node);
  }
  return accepts;
}

// A string of one to three random words of the sample, and sometimes one
// letter changed, so that some are woadwage words and some are not.
static int woadwage_test_random_string(const DictionaryWordList *words,
                                       const LetterDistribution *ld,
                                       uint64_t *random_state,
                                       MachineLetter *letters) {
  const int word_count = dictionary_word_list_get_count(words);
  assert(word_count > 0);
  const int pieces = 1 + (int)(woadwage_test_random(random_state) % 3);
  int length = 0;
  for (int piece_idx = 0; piece_idx < pieces; piece_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(
        words, (int)(woadwage_test_random(random_state) % word_count));
    const int word_length = dictionary_word_get_length(word);
    if (length + word_length > BOARD_DIM) {
      break;
    }
    for (int letter_idx = 0; letter_idx < word_length; letter_idx++) {
      letters[length++] = dictionary_word_get_word(word)[letter_idx];
    }
  }
  if (length > 0 && woadwage_test_random(random_state) % 3 == 0) {
    letters[woadwage_test_random(random_state) % length] =
        (MachineLetter)(1 + woadwage_test_random(random_state) %
                                (ld_get_size(ld) - 1));
  }
  return length;
}

static void test_woadwage_words(const KWG *words_kwg, const KWG *pairs_kwg,
                                const DictionaryWordList *words,
                                const DictionaryWordList *pairs,
                                const LetterDistribution *ld) {
  for (int pair_idx = 0; pair_idx < dictionary_word_list_get_count(pairs);
       pair_idx++) {
    const DictionaryWord *pair = dictionary_word_list_get_word(pairs, pair_idx);
    assert(kwg_accepts_woadwage(words_kwg, dictionary_word_get_word(pair),
                                dictionary_word_get_length(pair)));
  }
  uint64_t random_state = 0x9E3779B97F4A7C15ULL;
  int accepted = 0;
  for (int string_idx = 0; string_idx < WOADWAGE_TEST_RANDOM_STRINGS;
       string_idx++) {
    MachineLetter letters[BOARD_DIM];
    const int length =
        woadwage_test_random_string(words, ld, &random_state, letters);
    const bool expected =
        woadwage_test_dawg_contains(pairs_kwg, letters, length);
    assert(kwg_accepts_woadwage(words_kwg, letters, length) == expected);
    accepted += expected;
  }
  // Both answers come up often.
  assert(accepted > WOADWAGE_TEST_RANDOM_STRINGS / 5);
  assert(accepted < WOADWAGE_TEST_RANDOM_STRINGS * 4 / 5);
}

static void test_woadwage_cross_sets(const KWG *words_kwg, const KWG *pairs_kwg,
                                     const DictionaryWordList *words,
                                     const LetterDistribution *ld) {
  uint64_t random_state = 0xD1B54A32D192ED03ULL;
  int nonempty = 0;
  for (int case_idx = 0; case_idx < WOADWAGE_TEST_RANDOM_CROSS_SETS;
       case_idx++) {
    MachineLetter letters[BOARD_DIM];
    const int length =
        woadwage_test_random_string(words, ld, &random_state, letters);
    if (length < 2) {
      continue;
    }
    // The square replaces one letter of the string.
    const int hole = (int)(woadwage_test_random(&random_state) % length);
    uint64_t expected = 0;
    for (int ml = 1; ml < ld_get_size(ld); ml++) {
      letters[hole] = (MachineLetter)ml;
      if (woadwage_test_dawg_contains(pairs_kwg, letters, length)) {
        expected |= (uint64_t)1 << ml;
      }
    }
    assert(kwg_compute_woadwage_cross_set(words_kwg, letters, hole,
                                          letters + hole + 1,
                                          length - hole - 1) == expected);
    nonempty += expected != 0;
  }
  assert(nonempty > WOADWAGE_TEST_RANDOM_CROSS_SETS / 5);
}

static void woadwage_test_assert_same_cross_sets(const Game *woadwage_game,
                                                 const Game *classic_game) {
  const Board *woadwage_board = game_get_board(woadwage_game);
  const Board *classic_board = game_get_board(classic_game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int dir = 0; dir < 2; dir++) {
        assert(board_get_cross_set(woadwage_board, row, col, dir, 0) ==
               board_get_cross_set(classic_board, row, col, dir, 0));
        assert(board_get_cross_score(woadwage_board, row, col, dir, 0) ==
               board_get_cross_score(classic_board, row, col, dir, 0));
      }
    }
  }
}

// Generates every play of the record type, growing the list until it holds
// them all.
static MoveList *woadwage_test_generate(const Game *game,
                                        move_record_t move_record_type,
                                        const KWG *override_kwg) {
  const bool small = move_record_type == MOVE_RECORD_ALL_SMALL;
  int capacity = WOADWAGE_TEST_MOVE_CAPACITY;
  MoveList *moves =
      small ? move_list_create_small(capacity) : move_list_create(capacity);
  while (true) {
    const MoveGenArgs args = {
        .game = game,
        .move_list = moves,
        .move_record_type = move_record_type,
        .move_sort_type = small ? MOVE_SORT_SCORE : MOVE_SORT_EQUITY,
        .override_kwg = override_kwg,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&args);
    if (move_list_get_count(moves) < capacity) {
      return moves;
    }
    capacity *= 2;
    if (small) {
      small_move_list_destroy(moves);
      moves = move_list_create_small(capacity);
    } else {
      move_list_resize(moves, capacity);
    }
  }
}

static int woadwage_test_compare_small_moves(const void *a, const void *b) {
  const SmallMove *first = *(const SmallMove *const *)a;
  const SmallMove *second = *(const SmallMove *const *)b;
  if (first->tiny_move != second->tiny_move) {
    return first->tiny_move < second->tiny_move ? -1 : 1;
  }
  return (int)small_move_get_score(first) - (int)small_move_get_score(second);
}

// Asserts that both games have the same plays, and returns how many. The
// woadwage game generates them with override_kwg if it is not NULL.
static int woadwage_test_assert_same_moves(const Game *woadwage_game,
                                           const KWG *override_kwg,
                                           const Game *classic_game,
                                           Move *best_move) {
  MoveList *woadwage_moves =
      woadwage_test_generate(woadwage_game, MOVE_RECORD_ALL, override_kwg);
  MoveList *classic_moves =
      woadwage_test_generate(classic_game, MOVE_RECORD_ALL, NULL);
  const int count = move_list_get_count(classic_moves);
  assert(move_list_get_count(woadwage_moves) == count);
  SortedMoveList *woadwage_sorted = sorted_move_list_create(woadwage_moves);
  SortedMoveList *classic_sorted = sorted_move_list_create(classic_moves);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    assert(compare_moves(woadwage_sorted->moves[move_idx],
                         classic_sorted->moves[move_idx], true) == -1);
  }
  sorted_move_list_destroy(woadwage_sorted);
  sorted_move_list_destroy(classic_sorted);
  move_list_destroy(woadwage_moves);
  move_list_destroy(classic_moves);

  // The best play, which the shadow's bounds let generation stop short of
  // searching every anchor for.
  MoveList *woadwage_best =
      woadwage_test_generate(woadwage_game, MOVE_RECORD_BEST, override_kwg);
  MoveList *classic_best =
      woadwage_test_generate(classic_game, MOVE_RECORD_BEST, NULL);
  assert(compare_moves(move_list_get_move(woadwage_best, 0),
                       move_list_get_move(classic_best, 0), true) == -1);
  move_copy(best_move, move_list_get_move(classic_best, 0));
  move_list_destroy(woadwage_best);
  move_list_destroy(classic_best);

  // The endgame's plays.
  MoveList *woadwage_small = woadwage_test_generate(
      woadwage_game, MOVE_RECORD_ALL_SMALL, override_kwg);
  MoveList *classic_small =
      woadwage_test_generate(classic_game, MOVE_RECORD_ALL_SMALL, NULL);
  const int small_count = move_list_get_count(classic_small);
  assert(move_list_get_count(woadwage_small) == small_count);
  qsort(woadwage_small->small_moves, small_count, sizeof(SmallMove *),
        woadwage_test_compare_small_moves);
  qsort(classic_small->small_moves, small_count, sizeof(SmallMove *),
        woadwage_test_compare_small_moves);
  for (int move_idx = 0; move_idx < small_count; move_idx++) {
    assert(woadwage_test_compare_small_moves(
               &woadwage_small->small_moves[move_idx],
               &classic_small->small_moves[move_idx]) == 0);
  }
  small_move_list_destroy(woadwage_small);
  small_move_list_destroy(classic_small);
  return count;
}

// Plays seeded games in both lexica, checking every position. The odd games
// read woadwage's lexicon from gaddag_kwg, a GADDAG without a DAWG like the
// endgame's pruned KWGs, set on the game as the endgame sets them.
// Solves the position of game, whose bag is empty, in the config's lexicon and
// variant, and returns the best play's value.
static int woadwage_test_endgame_value(Config *config, const Game *game) {
  char *cgp = game_get_cgp(game, true);
  char *command = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, command);
  free(command);
  free(cgp);
  assert_config_exec_status(config, "endgame", ERROR_STATUS_SUCCESS);
  return endgame_results_get_value(config_get_endgame_results(config),
                                   ENDGAME_RESULT_BEST);
}

static void test_woadwage_games(const KWG *gaddag_kwg) {
  const char *const options =
      "-k1 CSW21 -k2 CSW21 -ld english -wmp false -wit false -s1 equity "
      "-s2 equity -r1 all -r2 all -numplays 1 -threads 1 -eplies 2 "
      "-ttfraction 0.001";
  char *woadwage_command = get_formatted_string(
      "set -lex %s -var woadwage %s", WOADWAGE_TEST_WORDS_LEXICON, options);
  char *classic_command = get_formatted_string(
      "set -lex %s -var classic %s", WOADWAGE_TEST_PAIRS_LEXICON, options);
  Config *woadwage_config = config_create_or_die(woadwage_command);
  Config *classic_config = config_create_or_die(classic_command);
  free(woadwage_command);
  free(classic_command);
  Game *woadwage_game = config_game_create(woadwage_config);
  Game *classic_game = config_game_create(classic_config);
  assert(game_get_variant(woadwage_game) == GAME_VARIANT_WOADWAGE);
  assert(game_get_variant(classic_game) == GAME_VARIANT_CLASSIC);
  Move best_move;
  int positions = 0;
  int plays = 0;
  int endgames = 0;
  for (int game_idx = 0; game_idx < WOADWAGE_TEST_GAMES; game_idx++) {
    const KWG *override_kwg = game_idx % 2 == 1 ? gaddag_kwg : NULL;
    if (override_kwg != NULL) {
      game_set_override_kwgs(woadwage_game, override_kwg, override_kwg,
                             DUAL_LEXICON_MODE_IGNORANT);
    } else {
      game_clear_override_kwgs(woadwage_game);
    }
    for (int game_number = 0; game_number < 2; game_number++) {
      Game *game = game_number == 0 ? woadwage_game : classic_game;
      game_reset(game);
      game_seed(game, 1000 + (uint64_t)game_idx);
      draw_starting_racks(game);
    }
    bool solve_endgame = ((WOADWAGE_TEST_ENDGAME_GAMES >> game_idx) & 1) != 0;
    for (int turn = 0;
         turn < WOADWAGE_TEST_MAX_TURNS && !game_over(classic_game); turn++) {
      const int player_index = game_get_player_on_turn_index(classic_game);
      assert(racks_are_equal(
          player_get_rack(game_get_player(woadwage_game, player_index)),
          player_get_rack(game_get_player(classic_game, player_index))));
      woadwage_test_assert_same_cross_sets(woadwage_game, classic_game);
      plays += woadwage_test_assert_same_moves(woadwage_game, override_kwg,
                                               classic_game, &best_move);
      positions++;
      // In the games WOADWAGE_TEST_ENDGAME_GAMES names, the first position
      // with an empty bag is solved both ways: the endgame searches woadwage
      // plays with its small-move generation and the incremental cross set
      // updates it plays them with.
      if (solve_endgame && bag_is_empty(game_get_bag(classic_game))) {
        solve_endgame = false;
        assert(woadwage_test_endgame_value(woadwage_config, woadwage_game) ==
               woadwage_test_endgame_value(classic_config, classic_game));
        assert(game_get_variant(config_get_game(woadwage_config)) ==
               GAME_VARIANT_WOADWAGE);
        endgames++;
      }
      play_move(&best_move, woadwage_game, NULL);
      play_move(&best_move, classic_game, NULL);
    }
    assert(game_over(woadwage_game) == game_over(classic_game));
  }
  printf("woadwage: %d positions, %d plays and %d endgames, the same in both "
         "lexica\n",
         positions, plays, endgames);
  assert(endgames > 0);
  game_destroy(woadwage_game);
  game_destroy(classic_game);
  config_destroy(woadwage_config);
  config_destroy(classic_config);
}

void test_woadwage(void) {
  DictionaryWordList *words = dictionary_word_list_create();
  DictionaryWordList *pairs = dictionary_word_list_create();
  woadwage_test_create_lexica(words, pairs);
  printf("woadwage: %d sampled words, %d words and pairs\n",
         dictionary_word_list_get_count(words),
         dictionary_word_list_get_count(pairs));

  const char *const options = "-k1 CSW21 -k2 CSW21 -ld english -wmp false";
  char *command = get_formatted_string("set -lex %s %s",
                                       WOADWAGE_TEST_WORDS_LEXICON, options);
  Config *words_config = config_create_or_die(command);
  free(command);
  command = get_formatted_string("set -lex %s %s", WOADWAGE_TEST_PAIRS_LEXICON,
                                 options);
  Config *pairs_config = config_create_or_die(command);
  free(command);
  const KWG *words_kwg =
      players_data_get_kwg(config_get_players_data(words_config), 0);
  const KWG *pairs_kwg =
      players_data_get_kwg(config_get_players_data(pairs_config), 0);
  const LetterDistribution *ld = config_get_ld(words_config);
  KWG *gaddag_kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG,
                                        KWG_MAKER_MERGE_EXACT);
  for (int kwg_idx = 0; kwg_idx < 2; kwg_idx++) {
    const KWG *kwg = kwg_idx == 0 ? words_kwg : gaddag_kwg;
    test_woadwage_words(kwg, pairs_kwg, words, pairs, ld);
    test_woadwage_cross_sets(kwg, pairs_kwg, words, ld);
  }
  config_destroy(words_config);
  config_destroy(pairs_config);

  test_woadwage_games(gaddag_kwg);
  kwg_destroy(gaddag_kwg);

  dictionary_word_list_destroy(words);
  dictionary_word_list_destroy(pairs);
  woadwage_test_remove_kwg(WOADWAGE_TEST_WORDS_LEXICON);
  woadwage_test_remove_kwg(WOADWAGE_TEST_PAIRS_LEXICON);
}
