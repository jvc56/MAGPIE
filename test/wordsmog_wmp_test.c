#include "wordsmog_wmp_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/wordsmog_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/words.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
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
  PLAY_GRAPH_TEST_GAMES = 2,
  PLAY_GRAPH_TEST_MAX_PLAYS = 100000,
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

// Every arrangement found by brute force: each order of the tiles, a blank as
// an undesignated tile, and then each designation of the blanks, ranked as
// wordsmog_best_arrangement ranks them and counted as
// wordsmog_count_arrangements counts them.
typedef struct BruteForce {
  const WordsmogSquare *squares;
  int num_squares;
  const WordsmogTiles *tiles;
  const MachineLetter *pins;
  int remaining[RACK_SIZE];
  int blanks_remaining;
  // Each square's tile: a natural letter, or WORDSMOG_PIN_BLANK.
  MachineLetter types[RACK_SIZE];
  MachineLetter current[RACK_SIZE];
  uint64_t arrangements;
  uint64_t plays;
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

// Each designation of the blanks in bf->types from square_idx on. Returns how
// many keep the main word valid.
static uint64_t brute_force_designate(BruteForce *bf, int square_idx) {
  if (square_idx == bf->num_squares) {
    if (!brute_force_designations_valid(bf)) {
      return 0;
    }
    brute_force_consider(bf);
    return 1;
  }
  if (bf->types[square_idx] != WORDSMOG_PIN_BLANK) {
    bf->current[square_idx] = bf->types[square_idx];
    return brute_force_designate(bf, square_idx + 1);
  }
  uint64_t count = 0;
  for (int ml = 1; ml <= ARRANGEMENT_TEST_LETTERS; ml++) {
    if (((bf->squares[square_idx].cross_set >> ml) & 1U) == 0) {
      continue;
    }
    bf->current[square_idx] = get_blanked_machine_letter((MachineLetter)ml);
    count += brute_force_designate(bf, square_idx + 1);
  }
  return count;
}

static void brute_force_place(BruteForce *bf, int square_idx) {
  if (square_idx == bf->num_squares) {
    const uint64_t designations = brute_force_designate(bf, 0);
    if (designations > 0) {
      bf->arrangements++;
      bf->plays += designations;
    }
    return;
  }
  const MachineLetter pin =
      bf->pins != NULL ? bf->pins[square_idx] : WORDSMOG_PIN_FREE;
  const uint64_t cross_set = bf->squares[square_idx].cross_set;
  for (int letter_idx = 0; letter_idx < bf->tiles->num_letters; letter_idx++) {
    const MachineLetter ml = bf->tiles->letters[letter_idx];
    if (bf->remaining[letter_idx] == 0 || ((cross_set >> ml) & 1U) == 0 ||
        (pin != WORDSMOG_PIN_FREE && pin != ml)) {
      continue;
    }
    bf->remaining[letter_idx]--;
    bf->types[square_idx] = ml;
    brute_force_place(bf, square_idx + 1);
    bf->remaining[letter_idx]++;
  }
  if (bf->blanks_remaining > 0 &&
      (pin == WORDSMOG_PIN_FREE || pin == WORDSMOG_PIN_BLANK)) {
    bf->blanks_remaining--;
    bf->types[square_idx] = WORDSMOG_PIN_BLANK;
    brute_force_place(bf, square_idx + 1);
    bf->blanks_remaining++;
  }
}

// wordsmog_best_arrangement and wordsmog_count_arrangements agree with brute
// force on random squares, tiles and pins: whether any arrangement is legal,
// the best one's score, adjustment and tiles, and how many there are with and
// without the blanks' designations.
static void test_wordsmog_arrangement_brute_force(void) {
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
    wordsmog_tiles_set_states(&tiles);
    // Half the cases pin some squares, to a letter or to a blank.
    MachineLetter pin_storage[RACK_SIZE];
    const MachineLetter *pins = NULL;
    if (test_random_below(&random_state, 2) == 0) {
      for (int square_idx = 0; square_idx < num_squares; square_idx++) {
        const int pick = test_random_below(&random_state, 10);
        pin_storage[square_idx] =
            pick < 6
                ? WORDSMOG_PIN_FREE
                : (pick < 9 ? (MachineLetter)(1 + test_random_below(
                                                      &random_state,
                                                      ARRANGEMENT_TEST_LETTERS))
                            : WORDSMOG_PIN_BLANK);
      }
      pins = pin_storage;
    }

    WordsmogArrangement arrangement;
    const bool found = wordsmog_best_arrangement(squares, num_squares, &tiles,
                                                 pins, &arrangement);
    uint64_t arrangements;
    uint64_t plays;
    wordsmog_count_arrangements(squares, num_squares, &tiles, pins,
                                &arrangements, &plays);

    BruteForce bf;
    memset(&bf, 0, sizeof(bf));
    bf.squares = squares;
    bf.num_squares = num_squares;
    bf.tiles = &tiles;
    bf.pins = pins;
    for (int letter_idx = 0; letter_idx < tiles.num_letters; letter_idx++) {
      bf.remaining[letter_idx] = tiles.counts[letter_idx];
    }
    bf.blanks_remaining = tiles.num_blanks;
    brute_force_place(&bf, 0);

    if (found != bf.found || arrangements != bf.arrangements ||
        plays != bf.plays ||
        (found &&
         (arrangement.score + arrangement.adjustment != bf.best_total ||
          arrangement.score != bf.best_score ||
          memcmp(arrangement.tiles, bf.best_tiles, (size_t)num_squares) !=
              0))) {
      log_fatal("arrangements differ from brute force in case %d", case_idx);
    }
    if (found) {
      num_legal++;
    }
  }
  printf("wordsmog arrangements: %d of %d cases legal, all matched brute "
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

// The best move by equity, from the exact PAT search when the term is on (see
// MoveGenArgs.wordsmog_exact_pat).
static const Move *get_top_equity_move_exact(Game *game, MoveList *move_list) {
  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_BEST,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .wordsmog_exact_pat = true};
  generate_moves(&args);
  return move_list_get_move(move_list, 0);
}

// Plays wordsmog games with the alpha generator and loads every position into
// a game that takes its cross sets, word validity and moves from the WMP: the
// cross sets, cross scores and the validity of each played move must agree,
// and so must the best move. With pat_command, both players of both games
// also take the PAT term, which the alpha generator prices on every
// arrangement and designation of every play and the WMP generator finds
// through its exact search (see WordsmogPATSearch); its default search, which
// prices fewer plays, finds a play no better, and the test reports how often
// it finds a worse one and by how much.
static void test_wordsmog_wmp_games(const char *pat_command, int num_games) {
  Config *alpha_config = create_alpha_config();
  Config *wmp_config = create_wmp_config();
  if (pat_command != NULL) {
    load_and_exec_config_or_die(alpha_config, pat_command);
    load_and_exec_config_or_die(wmp_config, pat_command);
  }
  load_and_exec_config_or_die(alpha_config, "new");
  load_and_exec_config_or_die(wmp_config, "new");
  Game *alpha_game = config_get_game(alpha_config);
  Game *wmp_game = config_get_game(wmp_config);
  MoveList *alpha_move_list = move_list_create(1);
  MoveList *wmp_move_list = move_list_create(1);
  MoveList *budget_move_list = move_list_create(1);
  MoveList *all_move_list = move_list_create(10);
  int positions = 0;
  int budget_misses = 0;
  int64_t budget_loss = 0;
  for (int game_idx = 0; game_idx < num_games; game_idx++) {
    game_reset(alpha_game);
    game_seed(alpha_game, WORDSMOG_WMP_TEST_SEED + game_idx);
    draw_starting_racks(alpha_game);
    while (game_get_game_end_reason(alpha_game) == GAME_END_REASON_NONE) {
      char *cgp = game_get_cgp(alpha_game, true);
      load_cgp_or_die(wmp_game, cgp);
      assert_cross_sets_match(alpha_game, wmp_game);
      const Move *alpha_move = get_top_equity_move(alpha_game, alpha_move_list);
      const Move *wmp_move = get_top_equity_move_exact(wmp_game, wmp_move_list);
      if (pat_command != NULL) {
        // A list of every play, one per group, starts with a play as good.
        const MoveGenArgs all_args = {.game = wmp_game,
                                      .move_list = all_move_list,
                                      .move_record_type = MOVE_RECORD_ALL,
                                      .move_sort_type = MOVE_SORT_EQUITY,
                                      .override_kwg = NULL,
                                      .eq_margin_movegen = 0,
                                      .target_equity = EQUITY_MAX_VALUE,
                                      .wordsmog_exact_pat = true};
        generate_moves(&all_args);
        move_list_sort_moves(all_move_list);
        assert(move_get_equity(move_list_get_move(all_move_list, 0)) ==
               move_get_equity(wmp_move));
        const Move *budget_move =
            get_top_equity_move(wmp_game, budget_move_list);
        assert(move_get_equity(budget_move) <= move_get_equity(wmp_move));
        if (move_get_equity(budget_move) < move_get_equity(wmp_move)) {
          budget_misses++;
          budget_loss +=
              move_get_equity(wmp_move) - move_get_equity(budget_move);
        }
      }
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
  printf("wordsmog wmp%s: %d positions with matching cross sets and best "
         "moves\n",
         pat_command != NULL ? " with PAT" : "", positions);
  assert(positions >= num_games * 10);
  if (pat_command != NULL) {
    printf("wordsmog wmp with PAT: the default search found a worse play in %d "
           "positions, by %.3f points on average over all positions\n",
           budget_misses, (double)budget_loss / 1000.0 / positions);
  }
  move_list_destroy(all_move_list);
  move_list_destroy(budget_move_list);
  move_list_destroy(wmp_move_list);
  move_list_destroy(alpha_move_list);
  config_destroy(wmp_config);
  config_destroy(alpha_config);
}

// A play's place and tiles, comparable with memcmp: with designate false, a
// blank is BLANK_MASK whatever it stands for, and with sort_tiles true, the
// placed tiles are sorted, so the key is the placement and tile multiset.
typedef struct PlayKey {
  uint8_t row;
  uint8_t col;
  uint8_t dir;
  uint8_t length;
  MachineLetter tiles[BOARD_DIM];
} PlayKey;

typedef struct PlayRecord {
  PlayKey key;
  Equity equity;
  Equity score;
} PlayRecord;

static int compare_machine_letters(const void *a, const void *b) {
  return (int)*(const MachineLetter *)a - (int)*(const MachineLetter *)b;
}

static void play_key_init(const Move *move, bool designate, bool sort_tiles,
                          PlayKey *key) {
  memset(key, 0, sizeof(*key));
  key->row = (uint8_t)move_get_row_start(move);
  key->col = (uint8_t)move_get_col_start(move);
  key->dir = (uint8_t)move_get_dir(move);
  key->length = (uint8_t)move_get_tiles_length(move);
  int num_tiles = 0;
  for (int tile_idx = 0; tile_idx < key->length; tile_idx++) {
    MachineLetter tile = move_get_tile(move, tile_idx);
    if (sort_tiles && tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (!designate && get_is_blanked(tile)) {
      tile = BLANK_MASK;
    }
    key->tiles[num_tiles++] = tile;
  }
  if (sort_tiles) {
    qsort(key->tiles, (size_t)num_tiles, sizeof(MachineLetter),
          compare_machine_letters);
  }
}

static int compare_play_records(const void *a, const void *b) {
  return memcmp(&((const PlayRecord *)a)->key, &((const PlayRecord *)b)->key,
                sizeof(PlayKey));
}

// The tile placement plays of move_list, keyed as play_key_init keys them and
// sorted by key. Returns how many.
static int play_records_create(const MoveList *move_list, bool designate,
                               bool sort_tiles, PlayRecord **records) {
  *records = malloc_or_die((size_t)(move_list_get_count(move_list) + 1) *
                           sizeof(PlayRecord));
  int num_records = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(move_list);
       move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    PlayRecord *record = &(*records)[num_records++];
    play_key_init(move, designate, sort_tiles, &record->key);
    record->equity = move_get_equity(move);
    record->score = move_get_score(move);
  }
  qsort(*records, (size_t)num_records, sizeof(PlayRecord),
        compare_play_records);
  return num_records;
}

// Keeps the record with the best equity, then score, of each run of equal
// keys. Returns how many are left.
static int play_records_keep_best_per_key(PlayRecord *records,
                                          int num_records) {
  int num_kept = 0;
  for (int record_idx = 0; record_idx < num_records; record_idx++) {
    if (num_kept > 0 &&
        memcmp(&records[num_kept - 1].key, &records[record_idx].key,
               sizeof(PlayKey)) == 0) {
      PlayRecord *kept = &records[num_kept - 1];
      if (records[record_idx].equity > kept->equity ||
          (records[record_idx].equity == kept->equity &&
           records[record_idx].score > kept->score)) {
        *kept = records[record_idx];
      }
      continue;
    }
    records[num_kept++] = records[record_idx];
  }
  return num_kept;
}

static const PlayRecord *play_records_find(const PlayRecord *records,
                                           int num_records,
                                           const PlayKey *key) {
  PlayRecord target = {.key = *key, .equity = 0, .score = 0};
  return bsearch(&target, records, (size_t)num_records, sizeof(PlayRecord),
                 compare_play_records);
}

static MoveList *generate_all_moves(Game *game, int capacity) {
  MoveList *move_list = move_list_create(capacity);
  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  return move_list;
}

// Checks one position's play graph, and the WMP's MOVE_RECORD_ALL list,
// against every play the alpha generator lists. Returns false when the
// position has more plays than max_plays, and is skipped.
static bool check_play_graph(Game *alpha_game, Game *wmp_game,
                             uint64_t max_plays, uint64_t *num_plays) {
  WordsmogPlayGraph *graph =
      wordsmog_play_graph_create(wmp_game, MOVE_SORT_EQUITY);
  uint64_t arrangements;
  uint64_t plays;
  wordsmog_play_graph_count(graph, &arrangements, &plays);
  *num_plays = plays;
  if (plays > max_plays) {
    wordsmog_play_graph_destroy(graph);
    return false;
  }
  // Every play the alpha generator lists, with room for exchanges and the
  // pass.
  MoveList *alpha_list = generate_all_moves(alpha_game, (int)plays + 1000);
  PlayRecord *alpha_plays;
  const int num_alpha_plays =
      play_records_create(alpha_list, true, false, &alpha_plays);
  assert((uint64_t)num_alpha_plays == plays);
  PlayRecord *alpha_arrangements;
  int num_alpha_arrangements =
      play_records_create(alpha_list, false, false, &alpha_arrangements);
  num_alpha_arrangements = play_records_keep_best_per_key(
      alpha_arrangements, num_alpha_arrangements);
  assert((uint64_t)num_alpha_arrangements == arrangements);

  // The graph lists each arrangement once, best first, with the equity of its
  // best designation, and each play is one the alpha generator lists.
  Move move;
  uint64_t num_listed = 0;
  Equity previous_equity = EQUITY_MAX_VALUE;
  while (wordsmog_play_graph_next(graph, &move)) {
    assert(move_get_equity(&move) <= previous_equity);
    previous_equity = move_get_equity(&move);
    PlayKey key;
    play_key_init(&move, true, false, &key);
    const PlayRecord *alpha_play =
        play_records_find(alpha_plays, num_alpha_plays, &key);
    assert(alpha_play != NULL);
    assert(alpha_play->equity == move_get_equity(&move));
    assert(alpha_play->score == move_get_score(&move));
    play_key_init(&move, false, false, &key);
    const PlayRecord *alpha_arrangement =
        play_records_find(alpha_arrangements, num_alpha_arrangements, &key);
    assert(alpha_arrangement != NULL);
    assert(alpha_arrangement->equity == move_get_equity(&move));
    num_listed++;
  }
  assert(num_listed == arrangements);
  wordsmog_play_graph_destroy(graph);

  // MOVE_RECORD_ALL with the WMP holds the best play of each placement and
  // tile multiset.
  PlayRecord *alpha_groups;
  int num_alpha_groups =
      play_records_create(alpha_list, false, true, &alpha_groups);
  num_alpha_groups =
      play_records_keep_best_per_key(alpha_groups, num_alpha_groups);
  MoveList *wmp_list = generate_all_moves(wmp_game, num_alpha_groups + 1000);
  int num_wmp_plays = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(wmp_list); move_idx++) {
    const Move *wmp_move = move_list_get_move(wmp_list, move_idx);
    if (move_get_type(wmp_move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    PlayKey key;
    play_key_init(wmp_move, true, false, &key);
    const PlayRecord *alpha_play =
        play_records_find(alpha_plays, num_alpha_plays, &key);
    assert(alpha_play != NULL);
    (void)alpha_play;
    play_key_init(wmp_move, false, true, &key);
    const PlayRecord *alpha_group =
        play_records_find(alpha_groups, num_alpha_groups, &key);
    assert(alpha_group != NULL);
    assert(alpha_group->equity == move_get_equity(wmp_move));
    assert(alpha_group->score == move_get_score(wmp_move));
    num_wmp_plays++;
  }
  assert(num_wmp_plays == num_alpha_groups);
  move_list_destroy(wmp_list);
  free(alpha_groups);
  free(alpha_arrangements);
  free(alpha_plays);
  move_list_destroy(alpha_list);
  return true;
}

// What test_wordsmog_play_graph checked.
typedef struct PlayGraphStats {
  int checked;
  int skipped;
  int with_blanks;
  uint64_t total_plays;
  uint64_t most_plays;
} PlayGraphStats;

// Loads cgp into both games and checks its play graph. Returns false, and
// checks nothing, when the position is not legal, as a rack whose tiles are
// not all left can make it.
static bool check_position(Game *alpha_game, Game *wmp_game, const char *cgp,
                           PlayGraphStats *stats) {
  ErrorStack *error_stack = error_stack_create();
  game_load_cgp(alpha_game, cgp, error_stack);
  if (error_stack_is_empty(error_stack)) {
    game_load_cgp(wmp_game, cgp, error_stack);
  }
  const bool legal = error_stack_is_empty(error_stack);
  error_stack_destroy(error_stack);
  if (!legal) {
    return false;
  }
  uint64_t num_plays;
  if (!check_play_graph(alpha_game, wmp_game, PLAY_GRAPH_TEST_MAX_PLAYS,
                        &num_plays)) {
    stats->skipped++;
    return true;
  }
  stats->checked++;
  stats->total_plays += num_plays;
  if (num_plays > stats->most_plays) {
    stats->most_plays = num_plays;
  }
  const Rack *rack = player_get_rack(
      game_get_player(alpha_game, game_get_player_on_turn_index(alpha_game)));
  if (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0) {
    stats->with_blanks++;
  }
  return true;
}

// cgp with the rack of the player on turn replaced by rack.
static char *cgp_with_rack(const char *cgp, const char *rack) {
  const char *racks = strchr(cgp, ' ');
  assert(racks != NULL);
  const char *other_rack = strchr(racks + 1, '/');
  assert(other_rack != NULL);
  return get_formatted_string("%.*s %s%s", (int)(racks - cgp), cgp, rack,
                              other_rack);
}

// The play graph and MOVE_RECORD_ALL against the alpha generator's full
// lists, on positions of alpha-played games with at most
// PLAY_GRAPH_TEST_MAX_PLAYS plays, and on each again with a small rack of
// blanks and a few letters, when its tiles are left.
static void test_wordsmog_play_graph(void) {
  static const char *const blank_racks[] = {"??",  "?AE",   "??ST",
                                            "?QU", "??EIN", "?"};
  const int num_blank_racks = (int)(sizeof(blank_racks) / sizeof(*blank_racks));
  Config *alpha_config = create_alpha_config();
  Config *check_config = create_alpha_config();
  Config *wmp_config = create_wmp_config();
  load_and_exec_config_or_die(alpha_config, "new");
  load_and_exec_config_or_die(check_config, "new");
  load_and_exec_config_or_die(wmp_config, "new");
  Game *alpha_game = config_get_game(alpha_config);
  Game *check_game = config_get_game(check_config);
  Game *wmp_game = config_get_game(wmp_config);
  MoveList *move_list = move_list_create(1);
  PlayGraphStats stats;
  memset(&stats, 0, sizeof(stats));
  int position_idx = 0;
  for (int game_idx = 0; game_idx < PLAY_GRAPH_TEST_GAMES; game_idx++) {
    game_reset(alpha_game);
    game_seed(alpha_game, WORDSMOG_WMP_TEST_SEED + game_idx);
    draw_starting_racks(alpha_game);
    while (game_get_game_end_reason(alpha_game) == GAME_END_REASON_NONE) {
      char *cgp = game_get_cgp(alpha_game, true);
      const bool legal = check_position(check_game, wmp_game, cgp, &stats);
      assert(legal);
      (void)legal;
      char *blank_cgp =
          cgp_with_rack(cgp, blank_racks[position_idx % num_blank_racks]);
      check_position(check_game, wmp_game, blank_cgp, &stats);
      free(blank_cgp);
      free(cgp);
      play_move(get_top_equity_move(alpha_game, move_list), alpha_game, NULL);
      position_idx++;
    }
  }
  printf("wordsmog play graph: %d positions (%d with a blank on the rack) "
         "matched the alpha lists, %llu plays in all, at most %llu; %d "
         "skipped as too large\n",
         stats.checked, stats.with_blanks,
         (unsigned long long)stats.total_plays,
         (unsigned long long)stats.most_plays, stats.skipped);
  assert(stats.checked >= PLAY_GRAPH_TEST_GAMES * 3);
  assert(stats.with_blanks >= PLAY_GRAPH_TEST_GAMES * 5);
  move_list_destroy(move_list);
  config_destroy(wmp_config);
  config_destroy(check_config);
  config_destroy(alpha_config);
}

// The output of a command that must succeed.
static char *command_output(Config *config, const char *command) {
  ErrorStack *error_stack = error_stack_create();
  config_load_command(config, command, error_stack);
  char *output = NULL;
  if (error_stack_is_empty(error_stack)) {
    config_run_str_api_command(config, error_stack, &output);
  }
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("command '%s' failed", command);
  }
  error_stack_destroy(error_stack);
  return output;
}

static void assert_output_has(const char *output, const char *expected) {
  if (strstr(output, expected) == NULL) {
    log_fatal("expected '%s' in:\n%s", expected, output);
  }
}

// The smog command shows what the play graph says, and rejects positions
// and arguments it cannot use.
static void test_wordsmog_smog_command(void) {
  Config *config = create_wmp_config();
  load_and_exec_config_or_die(config,
                              "load testdata/gcgs/success_long_game.gcg");
  load_and_exec_config_or_die(config, "goto 6");
  load_and_exec_config_or_die(config, "set -var wordsmog");
  load_and_exec_config_or_die(config, "rack ?EINRST");
  const Game *game = config_get_game(config);

  WordsmogPlayGraph *graph = wordsmog_play_graph_create(game, MOVE_SORT_EQUITY);
  uint64_t arrangements;
  uint64_t plays;
  wordsmog_play_graph_count(graph, &arrangements, &plays);
  const int groups = wordsmog_play_graph_rank_groups(graph);
  char *expected = get_formatted_string(
      "Groups (placement and tiles):     %d\n"
      "Arrangements:                     %llu\n"
      "Plays, every blank designation:   %llu\n",
      groups, (unsigned long long)arrangements, (unsigned long long)plays);
  char *output = command_output(config, "smog count");
  assert_output_has(output, expected);
  free(output);
  free(expected);

  // The best play, and the best play of the second group with a blank pinned
  // to its first square, as the graph gives them.
  Move move;
  StringBuilder *sb = string_builder_create();
  const bool has_play = wordsmog_play_graph_next(graph, &move);
  assert(has_play);
  (void)has_play;
  string_builder_add_move(sb, game_get_board(game), &move, game_get_ld(game),
                          false);
  // Under equity sort the graph ranks by static equity, which the tables
  // show in their StEq column.
  char *equity_str =
      get_formatted_string(" %8.2f", equity_to_double(move_get_equity(&move)));
  output = command_output(config, "smog 5");
  assert_output_has(output, string_builder_peek(sb));
  assert_output_has(output, "    5  ");
  assert_output_has(output, "Lv        Sc     StEq");
  assert_output_has(output, equity_str);
  free(output);
  free(equity_str);
  WordsmogPlayNode node;
  wordsmog_play_graph_get_group(graph, 1, &node);
  WordsmogPlayNode children[RACK_SIZE + 1];
  const int num_children =
      wordsmog_play_graph_get_children(graph, &node, children);
  int blank_child = 0;
  while (blank_child < num_children &&
         children[blank_child].pins[0] != WORDSMOG_PIN_BLANK) {
    blank_child++;
  }
  assert(blank_child < num_children);
  wordsmog_play_graph_get_play(graph, &children[blank_child], &move);
  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), &move, game_get_ld(game),
                          false);
  output = command_output(config, "smog group 2 ?");
  assert_output_has(output, "pinned: ? (1 of 7 squares)");
  assert_output_has(output, string_builder_peek(sb));
  free(output);
  output = command_output(config, "smog groups 3");
  assert_output_has(output, "    3  ");
  free(output);
  string_builder_destroy(sb);
  wordsmog_play_graph_destroy(graph);

  assert_config_exec_status(config, "smog group 0",
                            ERROR_STATUS_WORDSMOG_GRAPH_INVALID_ARG);
  assert_config_exec_status(config, "smog group 1 Q",
                            ERROR_STATUS_WORDSMOG_GRAPH_INVALID_ARG);
  assert_config_exec_status(config, "smog groups 0",
                            ERROR_STATUS_WORDSMOG_GRAPH_INVALID_ARG);
  assert_config_exec_status(config, "smog nonsense",
                            ERROR_STATUS_WORDSMOG_GRAPH_INVALID_ARG);
  load_and_exec_config_or_die(config, "set -var classic");
  assert_config_exec_status(config, "smog",
                            ERROR_STATUS_WORDSMOG_GRAPH_UNAVAILABLE);
  config_destroy(config);
}

// The play graph's speed on the positions of static WMP wordsmog games: per
// position, the best play from move generation, and from the graph its count
// and its first 1, 100 and 10000 plays. Run with "wsmogbench" on a release
// build.
void test_wordsmog_wmp_bench(void) {
  enum { BENCH_GAMES = 20, BENCH_STAGES = 5 };
  static const int stage_plays[BENCH_STAGES] = {0, 0, 1, 100, 10000};
  static const char *const stage_names[BENCH_STAGES] = {
      "best play (movegen)", "graph count", "graph first 1", "graph first 100",
      "graph first 10000"};
  Config *config = create_wmp_config();
  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(1);
  double seconds[BENCH_STAGES] = {0};
  uint64_t total_plays = 0;
  uint64_t most_plays = 0;
  int positions = 0;
  for (int game_idx = 0; game_idx < BENCH_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, WORDSMOG_WMP_TEST_SEED + game_idx);
    draw_starting_racks(game);
    while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
      Timer timer;
      ctimer_start(&timer);
      const Move *best = get_top_equity_move(game, move_list);
      seconds[0] += ctimer_elapsed_seconds(&timer);
      for (int stage = 1; stage < BENCH_STAGES; stage++) {
        ctimer_start(&timer);
        WordsmogPlayGraph *graph =
            wordsmog_play_graph_create(game, MOVE_SORT_EQUITY);
        if (stage == 1) {
          uint64_t arrangements;
          uint64_t plays;
          wordsmog_play_graph_count(graph, &arrangements, &plays);
          total_plays += plays;
          if (plays > most_plays) {
            most_plays = plays;
          }
        } else {
          Move move;
          for (int play_idx = 0; play_idx < stage_plays[stage] &&
                                 wordsmog_play_graph_next(graph, &move);
               play_idx++) {
            if (play_idx == 0 &&
                move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
              assert(move_get_equity(&move) == move_get_equity(best));
            }
          }
        }
        wordsmog_play_graph_destroy(graph);
        seconds[stage] += ctimer_elapsed_seconds(&timer);
      }
      play_move(best, game, NULL);
      positions++;
    }
  }
  printf("wordsmog graph bench: %d positions, %llu plays (with every "
         "designation), at most %llu in one position\n",
         positions, (unsigned long long)total_plays,
         (unsigned long long)most_plays);
  for (int stage = 0; stage < BENCH_STAGES; stage++) {
    printf("  %-20s %9.3f ms per position\n", stage_names[stage],
           1000.0 * seconds[stage] / positions);
  }
  move_list_destroy(move_list);
  config_destroy(config);
}

void test_wordsmog_wmp(void) {
  test_wordsmog_arrangement_brute_force();
  test_wordsmog_play_graph();
  test_wordsmog_smog_command();
  test_wordsmog_wmp_games(NULL, WORDSMOG_WMP_TEST_GAMES);
  test_wordsmog_wmp_games("set -pat CSW21", WORDSMOG_WMP_TEST_GAMES);
}
