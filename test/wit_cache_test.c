#include "wit_cache_test.h"

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
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/move_undo.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmp_maker.h"
#include "../src/impl/word_info_table_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CACHE_LEXICA[] = {"CSW21_wit_cache_a",
                                           "CSW21_wit_cache_b"};
static const char *const AT_POSITION =
    "15/15/15/15/15/15/15/7AT6/15/15/15/15/15/15/15 C/ 0/0 0";
static const char *const ATE_POSITION =
    "15/15/15/15/15/15/15/7ATE5/15/15/15/15/15/15/15 C/ 0/0 0";

// Generate tiny, matching KWG/WMP/WIT fixtures so these regressions run in CI
// without requiring a downloaded or production-sized WIT.
static Config *create_cache_config(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wit false");
  const LetterDistribution *ld = config_get_ld(config);
  const char *const words_by_lexicon[][4] = {{"AT", "ATE", "CAT", "CATS"},
                                             {"AT", "ATE", "BAT", "BATS"}};
  ErrorStack *error_stack = error_stack_create();
  for (int lexicon_idx = 0; lexicon_idx < 2; lexicon_idx++) {
    DictionaryWordList *words = dictionary_word_list_create();
    for (int word_idx = 0; word_idx < 4; word_idx++) {
      const char *word = words_by_lexicon[lexicon_idx][word_idx];
      const int length = (int)strlen(word);
      MachineLetter letters[BOARD_DIM];
      ld_str_to_mls(ld, word, false, letters, length);
      dictionary_word_list_add_word(words, letters, length);
    }
    dictionary_word_list_sort(words);
    KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                   KWG_MAKER_MERGE_EXACT);
    WMP *wmp = make_wmp_from_words(words, ld, 1);
    WordInfoTable *wit = make_word_info_table_from_words(words);
    char *kwg_path = data_filepaths_get_writable_filename(
        DEFAULT_TEST_DATA_PATH, CACHE_LEXICA[lexicon_idx],
        DATA_FILEPATH_TYPE_KWG, error_stack);
    char *wmp_path = data_filepaths_get_writable_filename(
        DEFAULT_TEST_DATA_PATH, CACHE_LEXICA[lexicon_idx],
        DATA_FILEPATH_TYPE_WORDMAP, error_stack);
    char *wit_path = data_filepaths_get_writable_filename(
        DEFAULT_TEST_DATA_PATH, CACHE_LEXICA[lexicon_idx],
        DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
    assert(error_stack_is_empty(error_stack));
    kwg_write_to_file(kwg, kwg_path, error_stack);
    wmp_write_to_file(wmp, wmp_path, error_stack);
    word_info_table_write_to_file(wit, wit_path, error_stack);
    assert(error_stack_is_empty(error_stack));
    free(kwg_path);
    free(wmp_path);
    free(wit_path);
    kwg_destroy(kwg);
    wmp_destroy(wmp);
    word_info_table_destroy(wit);
    dictionary_word_list_destroy(words);
  }
  error_stack_destroy(error_stack);
  load_and_exec_config_or_die(
      config, "set -lex CSW21_wit_cache_a -k1 CSW21 -k2 CSW21 -ld english "
              "-wmp true -wit true -s1 score -s2 score -r1 all -r2 all "
              "-numplays 10000");
  return config;
}

static void destroy_cache_config(Config *config) {
  config_destroy(config);
  const data_filepath_t types[] = {DATA_FILEPATH_TYPE_KWG,
                                   DATA_FILEPATH_TYPE_WORDMAP,
                                   DATA_FILEPATH_TYPE_WORD_INFO_TABLE};
  ErrorStack *error_stack = error_stack_create();
  for (int lexicon_idx = 0; lexicon_idx < 2; lexicon_idx++) {
    for (int type_idx = 0; type_idx < 3; type_idx++) {
      char *path = data_filepaths_get_writable_filename(
          DEFAULT_TEST_DATA_PATH, CACHE_LEXICA[lexicon_idx], types[type_idx],
          error_stack);
      assert(error_stack_is_empty(error_stack));
      remove_or_die(path);
      free(path);
    }
  }
  error_stack_destroy(error_stack);
}

static MoveList *generate_cache_moves(const Game *game) {
  int capacity = 10000;
  MoveList *moves = move_list_create(capacity);
  const MoveGenArgs args = {
      .game = game,
      .move_list = moves,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves_for_game(&args);
  // A capped list would not establish equality of the complete move sets.
  while (move_list_get_count(moves) == capacity) {
    capacity *= 2;
    move_list_resize(moves, capacity);
    generate_moves_for_game(&args);
  }
  return moves;
}

static void assert_same_moves(const Game *actual, const Game *expected) {
  MoveList *actual_moves = generate_cache_moves(actual);
  MoveList *expected_moves = generate_cache_moves(expected);
  assert(move_list_get_count(actual_moves) ==
         move_list_get_count(expected_moves));
  SortedMoveList *actual_sorted = sorted_move_list_create(actual_moves);
  SortedMoveList *expected_sorted = sorted_move_list_create(expected_moves);
  for (int move_idx = 0; move_idx < expected_sorted->count; move_idx++) {
    assert(compare_moves(actual_sorted->moves[move_idx],
                         expected_sorted->moves[move_idx], true) == -1);
  }
  sorted_move_list_destroy(actual_sorted);
  sorted_move_list_destroy(expected_sorted);
  move_list_destroy(actual_moves);
  move_list_destroy(expected_moves);
}

static void assert_cat(const Game *game) {
  ValidatedMoves *validated = validated_moves_create_and_assert_status(
      game, game_get_player_on_turn_index(game), "8G CAT", false, false,
      ERROR_STATUS_SUCCESS);
  MoveList *moves = generate_cache_moves(game);
  bool found = false;
  for (int move_idx = 0; move_idx < move_list_get_count(moves); move_idx++) {
    if (compare_moves_without_equity(move_list_get_move(moves, move_idx),
                                     validated_moves_get_move(validated, 0),
                                     true) == -1) {
      found = true;
    }
  }
  assert(found);
  move_list_destroy(moves);
  validated_moves_destroy(validated);
}

void test_wit_cache_copy(void) {
  Config *config = create_cache_config();
  Game *source = config_game_create(config);
  load_cgp_or_die(source, AT_POSITION);
  Game *destination = game_duplicate(source);
  // Copy twice into the same object: its first position has an incompatible
  // ATE row which must not survive the second copy's AT position.
  load_cgp_or_die(destination, ATE_POSITION);
  assert_cat(source);
  game_copy(destination, source);
  assert_cat(destination);
  assert_same_moves(destination, source);
  // A board copy preserves transposition, while invalidation covers both
  // orientations and both cross-set lanes.
  board_transpose(game_get_board(source));
  load_cgp_or_die(destination, ATE_POSITION);
  game_copy(destination, source);
  assert(game_get_board(destination)->transposed ==
         game_get_board(source)->transposed);
  board_transpose(game_get_board(destination));
  board_transpose(game_get_board(source));
  assert_same_moves(destination, source);
  game_destroy(destination);
  game_destroy(source);
  destroy_cache_config(config);
}

void test_wit_cache_undo(void) {
  Config *config = create_cache_config();
  for (int incremental = 0; incremental < 2; incremental++) {
    Game *game = config_game_create(config);
    load_cgp_or_die(
        game, "15/15/15/15/15/15/15/7AT6/15/15/15/15/15/15/15 CE/B 0/0 0");
    if (incremental) {
      Bag *bag = game_get_bag(game);
      while (bag_get_letters(bag) > 0) {
        bag_draw_random_letter(bag, 0);
      }
    } else {
      game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
    }
    ValidatedMoves *validated = validated_moves_create_and_assert_status(
        game, 0, "8H ATE", false, false, ERROR_STATUS_SUCCESS);
    const Move *move = validated_moves_get_move(validated, 0);
    if (incremental) {
      MoveUndo undo;
      play_move_incremental(move, game, &undo);
      update_cross_set_for_move_from_undo(&undo, game);
      unplay_move_incremental(game, &undo);
    } else {
      play_move(move, game, NULL);
      game_unplay_last_move(game);
    }
    rack_set_to_string(game_get_ld(game),
                       player_get_rack(game_get_player(game, 0)), "C");
    Game *fresh = game_duplicate(game);
    game_gen_all_cross_sets(fresh);
    assert_cat(game);
    assert_same_moves(game, fresh);
    game_destroy(fresh);
    validated_moves_destroy(validated);
    game_destroy(game);
  }
  destroy_cache_config(config);
}

void test_wit_cache_config(void) {
  Config *config = create_cache_config();
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/7AT6/15/15/15/15/15/15/15 C/C 0/0 0");
  const char *const transitions[] = {
      "set -wit false",
      "set -wit true",
      "set -wit1 false",
      "set -wit1 true -wit2 false",
      "set -wit true",
      "set -lex CSW21_wit_cache_b",
      "set -lex CSW21_wit_cache_a",
      "set -lex CSW21_wit_cache_b -wit false",
      "set -lex CSW21_wit_cache_a -wit true",
      "set -l2 CSW21_wit_cache_b",
      "set -l1 CSW21_wit_cache_b -l2 CSW21_wit_cache_a",
      "set -lex CSW21_wit_cache_a",
  };
  for (size_t step = 0; step < sizeof(transitions) / sizeof(transitions[0]);
       step++) {
    // Exercise a reused external game too; its owner updates it after changing
    // Config's borrowed lexical data, just as config_init_game updates its
    // game.
    Game *reused = game_duplicate(config_get_game(config));
    game_gen_all_cross_sets(reused);
    load_and_exec_config_or_die(config, transitions[step]);
    load_and_exec_config_or_die(config, "gen");
    const GameArgs game_args = {
        .players_data = config_get_players_data(config),
        .board_layout = config_get_board_layout(config),
        .ld = config_get_ld(config),
        .bingo_bonus = config_get_bingo_bonus(config),
        .game_variant = config_get_game_variant(config),
        .seed = config_get_seed(config),
    };
    game_update(reused, &game_args);
    Game *fresh = game_duplicate(config_get_game(config));
    game_gen_all_cross_sets(fresh);
    for (int player_idx = 0; player_idx < 2; player_idx++) {
      game_set_player_on_turn_index(config_get_game(config), player_idx);
      game_set_player_on_turn_index(reused, player_idx);
      game_set_player_on_turn_index(fresh, player_idx);
      assert_same_moves(config_get_game(config), fresh);
      assert_same_moves(reused, fresh);
    }
    game_set_player_on_turn_index(config_get_game(config), 0);
    game_destroy(fresh);
    game_destroy(reused);
    // Repopulate real rows so the next transition cannot accidentally pass
    // just because the preceding copy or configuration update cleared them.
    game_gen_all_cross_sets(config_get_game(config));
  }
  // Reloading the same named table can reuse the old allocation's address.
  // Invalidation must not depend on a pointer comparison noticing the change.
  ErrorStack *error_stack = error_stack_create();
  players_data_reload(config_get_players_data(config), PLAYERS_DATA_TYPE_WIT,
                      config_get_data_paths(config), error_stack);
  assert(error_stack_is_empty(error_stack));
  load_and_exec_config_or_die(config, "gen");
  assert_cat(config_get_game(config));
  error_stack_destroy(error_stack);
  destroy_cache_config(config);
}

void test_wit_cache(void) {
  test_wit_cache_copy();
  test_wit_cache_undo();
  test_wit_cache_config();
}

// On-demand integration coverage: build CSW24.wit first (make release does so).
// Compare every generated move, score, and equity through seeded games, and
// repeat the comparison after warming a reused copy and undoing each play.
void test_wit_cache_differential(void) {
  Config *with_wit = config_create_or_die(
      "set -lex CSW24 -wmp true -wit true -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 10000");
  Config *without_wit = config_create_or_die(
      "set -lex CSW24 -wmp true -wit false -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 10000");
  Game *game = config_game_create(with_wit);
  Game *reference = config_game_create(without_wit);
  Game *reused = game_duplicate(game);
  MoveList *best_move = move_list_create(1);
  int positions = 0;
  for (int game_idx = 0; game_idx < 32; game_idx++) {
    game_reset(game);
    game_reset(reference);
    const uint64_t seed = 0x610U + (uint64_t)game_idx;
    game_seed(game, seed);
    game_seed(reference, seed);
    draw_starting_racks(game);
    draw_starting_racks(reference);
    while (!game_over(game)) {
      assert_same_moves(game, reference);
      game_gen_all_cross_sets(reused);
      game_copy(reused, game);
      assert_same_moves(reused, reference);
      const Move move = *get_top_equity_move(game, best_move);
      game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
      play_move(&move, game, NULL);
      game_gen_all_cross_sets(game);
      game_unplay_last_move(game);
      assert_same_moves(game, reference);
      game_set_backup_mode(game, BACKUP_MODE_OFF);
      play_move(&move, game, NULL);
      play_move(&move, reference, NULL);
      positions++;
    }
    assert(game_over(reference));
    assert_games_are_equal(game, reference, true);
  }
  printf("WIT differential: 32 games, %d positions; identical moves, scores, "
         "and equities before/after copy and undo\n",
         positions);
  move_list_destroy(best_move);
  game_destroy(reused);
  game_destroy(reference);
  game_destroy(game);
  config_destroy(without_wit);
  config_destroy(with_wit);
}
