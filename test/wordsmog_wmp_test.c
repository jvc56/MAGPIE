#include "wordsmog_wmp_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/words.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/wordsmog_arrangement.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WORDSMOG_WMP_TEST_GAMES = 4,
  WORDSMOG_WMP_TEST_SEED = 20261026,
  // The brute-force check's letters are 1 .. ARRANGEMENT_TEST_LETTERS.
  ARRANGEMENT_TEST_LETTERS = 5,
  ARRANGEMENT_TEST_CASES = 20000,
};

// The alpha generator, which needs no WMP, is the reference.
static Config *create_alpha_config(void) {
  return config_create_or_die(
      "set -lex CSW21_alpha -wmp false -var wordsmog -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1 -threads 1");
}

static Config *create_wmp_config(void) {
  return config_create_or_die(
      "set -lex CSW21 -wmp true -var wordsmog -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1 -threads 1");
}

// A small deterministic generator for the brute-force check.
static uint64_t test_random_next(uint64_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

static int test_random_below(uint64_t *state, int bound) {
  return (int)(test_random_next(state) % (uint64_t)bound);
}

// The best arrangement found by trying every order of the tiles and every
// blank designation, ranked as wordsmog_best_arrangement ranks them.
typedef struct BruteForce {
  const WordsmogSquare *squares;
  int num_squares;
  const WordsmogTiles *tiles;
  int remaining[RACK_SIZE];
  int blanks_remaining;
  MachineLetter current[RACK_SIZE];
  bool found;
  Equity best_total;
  Equity best_score;
  MachineLetter best_tiles[RACK_SIZE];
} BruteForce;

static bool brute_force_designations_valid(const BruteForce *bf) {
  MachineLetter designations[2];
  int num_designations = 0;
  for (int square_idx = 0; square_idx < bf->num_squares; square_idx++) {
    if (get_is_blanked(bf->current[square_idx])) {
      designations[num_designations++] =
          get_unblanked_machine_letter(bf->current[square_idx]);
    }
  }
  if (num_designations == 0) {
    return true;
  }
  if (num_designations == 1) {
    return ((bf->tiles->blank_letters >> designations[0]) & 1U) != 0;
  }
  const MachineLetter low =
      designations[0] < designations[1] ? designations[0] : designations[1];
  const MachineLetter high =
      designations[0] < designations[1] ? designations[1] : designations[0];
  return ((bf->tiles->pair_letters[low] >> high) & 1U) != 0;
}

static void brute_force_consider(BruteForce *bf) {
  if (!brute_force_designations_valid(bf)) {
    return;
  }
  Equity score = 0;
  Equity adjustment = 0;
  for (int square_idx = 0; square_idx < bf->num_squares; square_idx++) {
    const MachineLetter tile = bf->current[square_idx];
    const MachineLetter letter = get_unblanked_machine_letter(tile);
    Equity tile_score = bf->tiles->blank_score;
    if (!get_is_blanked(tile)) {
      for (int letter_idx = 0; letter_idx < bf->tiles->num_letters;
           letter_idx++) {
        if (bf->tiles->letters[letter_idx] == letter) {
          tile_score = bf->tiles->scores[letter_idx];
        }
      }
    }
    score += tile_score * bf->squares[square_idx].coefficient;
    if (((bf->tiles->vowels >> letter) & 1U) != 0) {
      adjustment += bf->squares[square_idx].vowel_adjustment;
    }
  }
  const Equity total = score + adjustment;
  bool better = !bf->found || total > bf->best_total ||
                (total == bf->best_total && score > bf->best_score);
  if (!better && total == bf->best_total && score == bf->best_score) {
    better = memcmp(bf->current, bf->best_tiles, (size_t)bf->num_squares) < 0;
  }
  if (better) {
    bf->found = true;
    bf->best_total = total;
    bf->best_score = score;
    memcpy(bf->best_tiles, bf->current, (size_t)bf->num_squares);
  }
}

static void brute_force_place(BruteForce *bf, int square_idx) {
  if (square_idx == bf->num_squares) {
    brute_force_consider(bf);
    return;
  }
  const uint64_t cross_set = bf->squares[square_idx].cross_set;
  for (int letter_idx = 0; letter_idx < bf->tiles->num_letters; letter_idx++) {
    const MachineLetter ml = bf->tiles->letters[letter_idx];
    if (bf->remaining[letter_idx] == 0 || ((cross_set >> ml) & 1U) == 0) {
      continue;
    }
    bf->remaining[letter_idx]--;
    bf->current[square_idx] = ml;
    brute_force_place(bf, square_idx + 1);
    bf->remaining[letter_idx]++;
  }
  if (bf->blanks_remaining > 0) {
    bf->blanks_remaining--;
    for (int ml = 1; ml <= ARRANGEMENT_TEST_LETTERS; ml++) {
      if (((cross_set >> ml) & 1U) == 0) {
        continue;
      }
      bf->current[square_idx] = get_blanked_machine_letter((MachineLetter)ml);
      brute_force_place(bf, square_idx + 1);
    }
    bf->blanks_remaining++;
  }
}

// wordsmog_best_arrangement agrees with brute force on random squares and
// tiles: whether any arrangement is legal, and the best one's score,
// adjustment and tiles.
static void test_wordsmog_best_arrangement_brute_force(void) {
  uint64_t random_state = WORDSMOG_WMP_TEST_SEED;
  const uint64_t letter_mask =
      ((1ULL << (ARRANGEMENT_TEST_LETTERS + 1)) - 1) & ~(uint64_t)1;
  int num_legal = 0;
  for (int case_idx = 0; case_idx < ARRANGEMENT_TEST_CASES; case_idx++) {
    const int num_squares = 1 + test_random_below(&random_state, RACK_SIZE);
    WordsmogSquare squares[RACK_SIZE];
    for (int square_idx = 0; square_idx < num_squares; square_idx++) {
      // Mostly permissive cross sets, so that many cases have arrangements.
      uint64_t cross_set = letter_mask;
      if (test_random_below(&random_state, 2) == 0) {
        cross_set = test_random_next(&random_state) & letter_mask;
      }
      squares[square_idx].cross_set = cross_set;
      squares[square_idx].coefficient =
          1 + test_random_below(&random_state, 12);
      squares[square_idx].vowel_adjustment =
          test_random_below(&random_state, 3) == 0
              ? -1000 * test_random_below(&random_state, 4)
              : 0;
    }
    WordsmogTiles tiles;
    memset(&tiles, 0, sizeof(tiles));
    tiles.num_blanks = test_random_below(&random_state, 4) == 0
                           ? 1 + test_random_below(&random_state, 2)
                           : 0;
    if (tiles.num_blanks > num_squares) {
      tiles.num_blanks = num_squares;
    }
    tiles.blank_score = 1000 * test_random_below(&random_state, 2);
    tiles.vowels = (1U << 1) | (1U << 4);
    int counts[ARRANGEMENT_TEST_LETTERS + 1] = {0};
    for (int tile_idx = 0; tile_idx < num_squares - tiles.num_blanks;
         tile_idx++) {
      counts[1 + test_random_below(&random_state, ARRANGEMENT_TEST_LETTERS)]++;
    }
    for (int ml = 1; ml <= ARRANGEMENT_TEST_LETTERS; ml++) {
      if (counts[ml] == 0) {
        continue;
      }
      tiles.letters[tiles.num_letters] = (MachineLetter)ml;
      tiles.counts[tiles.num_letters] = counts[ml];
      tiles.scores[tiles.num_letters] = 1000 * (ml % 4);
      tiles.num_letters++;
    }
    tiles.blank_letters =
        (uint32_t)(test_random_next(&random_state) & letter_mask);
    for (int low = 1; low <= ARRANGEMENT_TEST_LETTERS; low++) {
      tiles.pair_letters[low] =
          (uint32_t)(test_random_next(&random_state) & letter_mask) &
          ~((1U << low) - 1U);
    }

    WordsmogArrangement arrangement;
    const bool found =
        wordsmog_best_arrangement(squares, num_squares, &tiles, &arrangement);

    BruteForce bf;
    memset(&bf, 0, sizeof(bf));
    bf.squares = squares;
    bf.num_squares = num_squares;
    bf.tiles = &tiles;
    for (int letter_idx = 0; letter_idx < tiles.num_letters; letter_idx++) {
      bf.remaining[letter_idx] = tiles.counts[letter_idx];
    }
    bf.blanks_remaining = tiles.num_blanks;
    brute_force_place(&bf, 0);

    if (found != bf.found ||
        (found &&
         (arrangement.score + arrangement.adjustment != bf.best_total ||
          arrangement.score != bf.best_score ||
          memcmp(arrangement.tiles, bf.best_tiles, (size_t)num_squares) !=
              0))) {
      log_fatal("best arrangement differs from brute force in case %d",
                case_idx);
    }
    if (found) {
      num_legal++;
    }
  }
  printf("wordsmog best arrangement: %d of %d cases legal, all matched brute "
         "force\n",
         num_legal, ARRANGEMENT_TEST_CASES);
  assert(num_legal > ARRANGEMENT_TEST_CASES / 4);
}

static void assert_cross_sets_match(const Game *alpha_game,
                                    const Game *wmp_game) {
  const Board *alpha_board = game_get_board(alpha_game);
  const Board *wmp_board = game_get_board(wmp_game);
  for (int dir = 0; dir < 2; dir++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        const uint64_t alpha_cross_set =
            board_get_cross_set(alpha_board, row, col, dir, 0);
        const uint64_t wmp_cross_set =
            board_get_cross_set(wmp_board, row, col, dir, 0);
        if (alpha_cross_set != wmp_cross_set) {
          log_fatal("cross sets differ at row %d col %d dir %d: alpha %llx, "
                    "wmp %llx",
                    row, col, dir, (unsigned long long)alpha_cross_set,
                    (unsigned long long)wmp_cross_set);
        }
        assert(board_get_cross_score(alpha_board, row, col, dir, 0) ==
               board_get_cross_score(wmp_board, row, col, dir, 0));
      }
    }
  }
}

// The words a move forms on the WMP game's board are all valid by the WMP.
static void assert_move_valid_by_wmp(const Game *wmp_game, const Move *move) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const Player *player =
      game_get_player(wmp_game, game_get_player_on_turn_index(wmp_game));
  FormedWords *formed_words =
      formed_words_create(game_get_board(wmp_game), move);
  formed_words_populate_validities(player_get_kwg(player),
                                   player_get_wmp(player), formed_words, true);
  for (int word_idx = 0; word_idx < formed_words_get_num_words(formed_words);
       word_idx++) {
    assert(formed_words_get_word_valid(formed_words, word_idx));
  }
  formed_words_destroy(formed_words);
}

static bool moves_match(const Move *m1, const Move *m2) {
  if (move_get_type(m1) != move_get_type(m2) ||
      move_get_row_start(m1) != move_get_row_start(m2) ||
      move_get_col_start(m1) != move_get_col_start(m2) ||
      move_get_dir(m1) != move_get_dir(m2) ||
      move_get_tiles_played(m1) != move_get_tiles_played(m2) ||
      move_get_tiles_length(m1) != move_get_tiles_length(m2) ||
      move_get_score(m1) != move_get_score(m2) ||
      move_get_equity(m1) != move_get_equity(m2)) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(m1); tile_idx++) {
    if (move_get_tile(m1, tile_idx) != move_get_tile(m2, tile_idx)) {
      return false;
    }
  }
  return true;
}

static void print_move(const Game *game, const char *label, const Move *move) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  printf("%s: %s, equity %d\n", label, string_builder_peek(sb),
         move_get_equity(move));
  string_builder_destroy(sb);
}

// Plays wordsmog games with the alpha generator and loads every position into
// a game that takes its cross sets, word validity and moves from the WMP: the
// cross sets, cross scores and the validity of each played move must agree,
// and so must the best move.
static void test_wordsmog_wmp_games(void) {
  Config *alpha_config = create_alpha_config();
  Config *wmp_config = create_wmp_config();
  load_and_exec_config_or_die(alpha_config, "new");
  load_and_exec_config_or_die(wmp_config, "new");
  Game *alpha_game = config_get_game(alpha_config);
  Game *wmp_game = config_get_game(wmp_config);
  MoveList *alpha_move_list = move_list_create(1);
  MoveList *wmp_move_list = move_list_create(1);
  int positions = 0;
  for (int game_idx = 0; game_idx < WORDSMOG_WMP_TEST_GAMES; game_idx++) {
    game_reset(alpha_game);
    game_seed(alpha_game, WORDSMOG_WMP_TEST_SEED + game_idx);
    draw_starting_racks(alpha_game);
    while (game_get_game_end_reason(alpha_game) == GAME_END_REASON_NONE) {
      char *cgp = game_get_cgp(alpha_game, true);
      load_cgp_or_die(wmp_game, cgp);
      assert_cross_sets_match(alpha_game, wmp_game);
      const Move *alpha_move = get_top_equity_move(alpha_game, alpha_move_list);
      const Move *wmp_move = get_top_equity_move(wmp_game, wmp_move_list);
      if (!moves_match(alpha_move, wmp_move)) {
        printf("best moves differ in game %d at position %d:\n%s\n", game_idx,
               positions, cgp);
        print_move(alpha_game, "alpha", alpha_move);
        print_move(wmp_game, "wmp", wmp_move);
        log_fatal("wordsmog best moves differ");
      }
      free(cgp);
      assert_move_valid_by_wmp(wmp_game, alpha_move);
      play_move(alpha_move, alpha_game, NULL);
      positions++;
    }
  }
  printf("wordsmog wmp: %d positions with matching cross sets and best moves\n",
         positions);
  assert(positions >= WORDSMOG_WMP_TEST_GAMES * 10);
  move_list_destroy(wmp_move_list);
  move_list_destroy(alpha_move_list);
  config_destroy(wmp_config);
  config_destroy(alpha_config);
}

void test_wordsmog_wmp(void) {
  test_wordsmog_best_arrangement_brute_force();
  test_wordsmog_wmp_games();
}
