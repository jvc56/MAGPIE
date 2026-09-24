#include "word_prune_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/words.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/word_prune.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void test_possible_words(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  // empty board
  Game *game = config_game_create(config);
  DictionaryWordList *possible_word_list = dictionary_word_list_create();
  generate_possible_words(game, NULL, possible_word_list);

  // all words except unplayable (PIZZAZZ, etc.)
  // 24 of the 279077 words in CSW21 are not playable using a standard English
  // tile set. 17 have >3 Z's, 2 have >3 K's, 5 have >6 S's.
  assert(dictionary_word_list_get_count(possible_word_list) == 279053);
  const char zonule[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp_or_die(game, zonule);

  dictionary_word_list_clear(possible_word_list);
  generate_possible_words(game, NULL, possible_word_list);

  assert(dictionary_word_list_get_count(possible_word_list) == 62702);
  assert_word_count(game_get_ld(game), possible_word_list, "ENGUARDING", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "NONFACT", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "ZOOMED", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "SIGISBEO", 1);

  const char taurine[300] =
      "15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/WE3R1V7/"
      "AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 lex NWL20;";

  load_cgp_or_die(game, taurine);
  dictionary_word_list_clear(possible_word_list);
  generate_possible_words(game, NULL, possible_word_list);

  assert(dictionary_word_list_get_count(possible_word_list) == 6396);
  assert_word_count(game_get_ld(game), possible_word_list, "CLOVE", 0);
  assert_word_count(game_get_ld(game), possible_word_list, "SLOVE", 0);

  const char anoretic[300] =
      "AnORETIC7/7R7/2C1EMEU7/1JANNY1X7/2P12/1SIDELING6/2ZAG7Q2/3MOVED3AA2/"
      "6FEAL1IT2/2NEGATES2D3/3DOH2KEEFS2/WITH7UN2/I10LO2/LABOUR6B2/Y6POTTOS2 "
      "?AENORW/EIIIRUV 332/384 0 lex NWL20;";

  load_cgp_or_die(game, anoretic);
  dictionary_word_list_clear(possible_word_list);
  generate_possible_words(game, NULL, possible_word_list);

  assert(dictionary_word_list_get_count(possible_word_list) == 11161);
  assert_word_count(game_get_ld(game), possible_word_list, "AALII", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "REVERBERANT", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "ZIPWIRE", 1);
  dictionary_word_list_destroy(possible_word_list);
  game_destroy(game);
  config_destroy(config);
}

static bool word_list_contains(const DictionaryWordList *list,
                               const MachineLetter *word, int length) {
  int low = 0;
  int high = dictionary_word_list_get_count(list) - 1;
  while (low <= high) {
    const int mid = (low + high) / 2;
    const DictionaryWord *entry = dictionary_word_list_get_word(list, mid);
    const int entry_length = dictionary_word_get_length(entry);
    const int common = length < entry_length ? length : entry_length;
    int cmp = memcmp(dictionary_word_get_word(entry), word, (size_t)common);
    if (cmp == 0) {
      cmp = entry_length - length;
    }
    if (cmp == 0) {
      return true;
    }
    if (cmp < 0) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return false;
}

// Every word formed by every tile move in every position reachable within
// plies_left (all sequences, both players, full-lexicon move generation) must
// be in the pruned list. Passes form no words; the bag is empty so there are
// no exchanges. Returns the number of moves played.
static int assert_reachable_words_are_kept(Game *game,
                                           const DictionaryWordList *pruned,
                                           int plies_left) {
  if (plies_left == 0 ||
      game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
    return 0;
  }
  MoveList *moves = move_list_create(100000);
  const MoveGenArgs args = {.game = game,
                            .move_list = moves,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_SCORE,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .target_leave_size_for_exchange_cutoff =
                                UNSET_LEAVE_SIZE};
  generate_moves(&args);
  int moves_played = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(moves); move_idx++) {
    const Move *move = move_list_get_move(moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    FormedWords *formed = formed_words_create(game_get_board(game), move);
    for (int word_idx = 0; word_idx < formed_words_get_num_words(formed);
         word_idx++) {
      assert(
          word_list_contains(pruned, formed_words_get_word(formed, word_idx),
                             formed_words_get_word_length(formed, word_idx)));
    }
    formed_words_destroy(formed);
    play_move(move, game, NULL); // backs up the game itself
    moves_played++;
    moves_played +=
        assert_reachable_words_are_kept(game, pruned, plies_left - 1);
    game_unplay_last_move(game);
  }
  move_list_destroy(moves);
  return moves_played;
}

static void assert_cross_check_pruning(Game *game, int plies,
                                       int min_moves_expected) {
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));
  DictionaryWordList *plain = dictionary_word_list_create();
  DictionaryWordList *pruned = dictionary_word_list_create();
  generate_possible_words(game, kwg, plain);
  generate_possible_words_with_cross_checks(game, kwg, pruned);
  // A subset of the plain list.
  assert(dictionary_word_list_get_count(pruned) <=
         dictionary_word_list_get_count(plain));
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(pruned);
       word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(pruned, word_idx);
    assert(word_list_contains(plain, dictionary_word_get_word(word),
                              dictionary_word_get_length(word)));
  }
  // A superset of every word any reachable move can form.
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  const int moves_played = assert_reachable_words_are_kept(game, pruned, plies);
  assert(moves_played >= min_moves_expected);
  dictionary_word_list_destroy(pruned);
  dictionary_word_list_destroy(plain);
}

// With static leaves the depth-limited search is exact, so pruning with cross
// checks must reproduce the unpruned search's value and node count exactly.
static void
assert_endgame_unchanged_by_cross_check_pruning(const Config *config,
                                                Game *game, int plies) {
  int values[2];
  uint64_t nodes[2];
  for (int mode = 0; mode < 2; mode++) {
    EndgameCtx *solver = NULL;
    EndgameResults *results = endgame_results_create();
    ErrorStack *error_stack = error_stack_create();
    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = plies,
                        .tt_fraction_of_mem = 0.01,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = 1,
                        .num_top_moves = 1,
                        .use_heuristics = false,
                        .skip_word_pruning = mode == 1,
                        .seed = 42};
    endgame_solve(&solver, &args, results, error_stack);
    assert(error_stack_is_empty(error_stack));
    values[mode] = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
    nodes[mode] = endgame_ctx_get_nodes_searched(solver);
    endgame_ctx_destroy(solver);
    endgame_results_destroy(results);
    error_stack_destroy(error_stack);
  }
  assert(values[0] == values[1]);
  assert(nodes[0] == nodes[1]);
}

void test_possible_words_with_cross_checks(void) {
  Config *config = config_create_or_die("set -lex CSW21 -threads 1");
  Game *game = config_game_create(config);

  // Every reachable position of two small endgames (whole game trees).
  const char tiny_a[400] =
      "7Q6B/BEENTO1u6U/5READVISE1K/7Y5WE/4VAUDOO3I1/5X2HUP2L1/6G3E2LI/"
      "T4ARChERY1EF/I5A3I2R1/FORAGING2DANS1/T2HUNDO2OMA2/1JAIL1S3TON2/6I3EWE2/"
      "COMPEER4T3/7ZATIS3 N/L 380/528 0 lex CSW21;";
  load_cgp_or_die(game, tiny_a);
  assert_cross_check_pruning(game, 24, 10);
  assert_endgame_unchanged_by_cross_check_pruning(config, game, 24);

  const char tiny_b[400] =
      "6WILGA4/10BRAND/5TOFU3R2/6MANDATES1/9I2N1E/5BUNGY1GI1V/7E1A1AT1O/"
      "6HUP2PETS/5DIRE1QI1R1/1JURA2O3E1O1/3ELVaNITEs1L1/MINX2DIF2T1L1/E6C5E1/"
      "ZO9KARA/EW7COYISH ES/OO 494/436 0 lex CSW21;";
  load_cgp_or_die(game, tiny_b);
  assert_cross_check_pruning(game, 24, 10);
  assert_endgame_unchanged_by_cross_check_pruning(config, game, 24);

  // Every first move of a fourteen-tile endgame, and a depth-two exact search.
  const char full_racks[400] =
      "7F3QINS/7E3E2H/6ORBITED1O/3JAMBU2OM2R/1ROATE1L2FAV1T/GI5ED1T1OPE/"
      "OD6R3WEN/EL5RUNTY2S/1E6N2U3/1Y5AKA1C3/ES1VAgUIsH1C3/X2I7A3/I2G11/"
      "LOOING9/E2A11 ADLRSTZ/EIINPW 451/296 0 lex CSW21;";
  load_cgp_or_die(game, full_racks);
  assert_cross_check_pruning(game, 1, 100);
  assert_endgame_unchanged_by_cross_check_pruning(config, game, 2);

  // The existing fixtures: the cross-check list is never larger.
  const char taurine[300] =
      "15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/WE3R1V7/"
      "AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 lex CSW21;";
  load_cgp_or_die(game, taurine);
  assert_cross_check_pruning(game, 1, 50);

  game_destroy(game);
  config_destroy(config);
}

void test_word_prune(void) {
  test_possible_words();
  test_possible_words_with_cross_checks();
}