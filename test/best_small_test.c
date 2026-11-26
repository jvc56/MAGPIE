#include "best_small_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/lane_shadow_cache.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
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

// With best_small_canonical_ties, MOVE_RECORD_BEST_SMALL must return exactly
// the argmax of a full MOVE_RECORD_ALL_SMALL generation under one rule: rank a
// play by its score plus best_small_out_bonus when it uses the whole rack,
// break equal ranks in favor of the larger tiny_move, and return a pass only
// when there is no play. The result must not depend on the order in which the
// shadow-pruned search visits anchors and plays. Without it, the result must
// still be a play of the best rank. MOVE_RECORD_BEST_SMALL_FACE must do the
// same with a play's rank also counting best_small_face_weight times the face
// value of the tiles it places.

enum {
  // The lane shadow cache's anchor plays (check_twin_lane_boards).
  TWIN_LANE_FULL_RACKS_PER_BOARD = 18,
  TWIN_LANE_SEED = 2000,
  TWIN_LANE_CACHE_INDEX_BITS = 12,
  TWIN_LANE_PASSES = 2,
  ALL_SMALL_CAPACITY = 250000,
  SELF_PLAY_GAMES_PER_LEXICON = 6,
  SELF_PLAY_MAX_MOVES = 120,
  SELF_PLAY_CANDIDATE_MOVES = 6,
  BONUS_KINDS = 3,
  // The face weights checked: none (MOVE_RECORD_BEST_SMALL), the weight the
  // endgame's static leaf playouts use, and a larger one.
  FACE_WEIGHT_KINDS = 3,
  SELF_PLAY_BASE_SEED = 1000,
  // The lane shadow cache check: its self-play games, and its caches' 2^16
  // lanes (4 MB) and 2^16 anchor plays (2 MB) each, more than those games
  // record.
  LANE_SHADOW_TEST_GAMES = 4,
  LANE_SHADOW_TEST_INDEX_BITS = 16,
  // The generations with a pool (check_pool_generations): its self-play
  // games, the racks drawn from each position's pool, and the out bonus.
  POOL_TEST_GAMES = 4,
  POOL_TEST_RACKS = 10,
  // The last of those racks hold one blank and two, from the wider pool.
  POOL_TEST_BLANK_RACKS = 2,
  POOL_TEST_OUT_BONUS = 100000,
};

typedef struct BestSmallStats {
  int positions;
  int checks;
  int multiway_ties;
  int pass_results;
  int out_bonus_wins;
  // Checks with a face weight where the play found is not the play the same
  // check without the weight finds.
  int face_weight_changes;
} BestSmallStats;

static const int face_weights[FACE_WEIGHT_KINDS] = {0, 2, 5};

// The rank of a play of a rack of `rack_size` tiles: its score, plus `bonus`
// when it uses the whole rack, plus `face_weight` times the face value of the
// tiles it places, a blank counting nothing.
static Equity small_move_rank(const SmallMove *small_move,
                              const LetterDistribution *ld, int rack_size,
                              Equity bonus, int face_weight) {
  Equity value = int_to_equity(small_move_get_score(small_move));
  const int tiles_played = small_move_get_tiles_played(small_move);
  if (tiles_played == rack_size) {
    value += bonus;
  }
  for (int tile_idx = 0; tile_idx < tiles_played; tile_idx++) {
    if (!small_move_tile_is_blank(small_move, tile_idx)) {
      value += face_weight *
               ld_get_score(ld, small_move_get_tile(small_move, tile_idx));
    }
  }
  return value;
}

// The distinct CGPs used by test/endgame_test.c.
static const char *const endgame_cgps[] = {
    "AIDER2U7/b1E1E2N1Z5/AWN1T2M1ATT3/LI1COBLE2OW3/OP2U2E2AA3/NE2CUSTARDS1Q1/"
    "ER1OH5I2U1/S2K2FOB1ERGOT/5HEXYLS2I1/4JIN6N1/2GOOP2NAIVEsT/1DIRE10/"
    "2GAY10/15/15 AEFILMR/DIV 371/412 0 -lex NWL20",
    "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
    "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
    "3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21",
    "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
    "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
    "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20",
    "7F3QINS/7E3E2H/6ORBITED1O/3JAMBU2OM2R/1ROATE1L2FAV1T/GI5ED1T1OPE/"
    "OD6R3WEN/EL5RUNTY2S/1E6N2U3/1Y5AKA1C3/ES1VAgUIsH1C3/X2I7A3/I2G11/"
    "LOOING9/E2A11 ADLRSTZ/EIINPW 451/296 0 -lex CSW24",
    "11CLAG/9BRAAI1/11N3/11D3/10BOW2/10ERA2/6A3A1R2/C2WEDDInGS1P1I/"
    "U1VOX1O3T3N/RHO1E1PERILUNES/FEM1A1T3Y3O/1HINT1I7U/2TO2V2GEED1L/"
    "1AYU2ENTITLES1/QI1N3OAF1KEiR EIOS/JMZ 318/517 0 -lex CSW21",
    "14C/13QI/12FIE/10VEE1R/9KIT2G/8CIG1IDE/8UTA2AS/7ST1SYPh1/6JA5A1/"
    "5WOLD2BOBA/3PLOT1R1NU1EX/Y1VEIN1NOR1mOA1/UT1AT1N1L2FEH1/GUR2WIRER5/"
    "SNEEZED8 ADENOOO/AHIILMM 353/236 0 -lex CSW21",
    "4EXODE6/1DOFF1KERATIN1U/1OHO8YEN/1POOJA1B3MEWS/5SQUINTY2A/4RHINO1e3V/"
    "2B4C2R3E/GOAT1D1E2ZIN1d/1URACILS2E4/1PIG1S4T4/2L2R4T4/2L2A1GENII3/"
    "2A2T1L7/5E1A7/5D1M7 AEEIRUW/V 410/409 0 -lex CSW21",
    "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/4DONG1OR1R1i/"
    "7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/EW1ATAP2E1G3/M10U3/D3PATOOTIE3/15/15/"
    "15 ?AEEKSU/BEIQUVW 276/321 0 -lex NWL23",
    "2ABERRaNT4G/5HE6FE/5OS5CUR/5DI5OMA/1HARPIST4WEN/5ET2Q3TI/6E1VIVO2A/"
    "4JUDY2ABOIL/4I2ERASERS1/1DOWLY8M/4TaLEGGIO2I/7Z6C/7I3PUNK/7NONTONAL/"
    "7E2AX2E AD/EFU 467/497 0 -lex CSW21",
    "W5JOE3V2/O5AMNIONIC1/WRiNKLY5B2/S6QUELLS2/10A4/5fATIGUE1UT/4POZ3R2NA/"
    "3CEBOID1E2GI/3A6A2IT/3P6T2R1/3R6EH1D1/3O7UGS1/3A7MA2/3T2LIEF1FE2/"
    "3E1HINDERS3 INOORTY/ADEEVX 372/532 0 -lex CSW21",
    "5LEX1AFFORD/3SNOWIER1Y3/2V8T3/1DO6J1T3/1AG6OKE3/1U7YE3N/1B8T3E/"
    "ERICA2GARTH2V/1I2MIAOW1L3U/PE6AZIONES/OS8n4/L6DINGBATS/I14/COULEE9/"
    "E3HARN7 ADEIIU?/MNPQRT 369/399 0 -lex CSW21",
    "F6ENDEW2G/Y4AUA2TELCO/ROTARY8R/D2MEARING4I/2KITH3OW4/1QIN5XI4/1U6B1V4/"
    "DA1DAUPhINE4/AL1H4Z6/EM1U3J7/3T3E7/APRICATE7/VOE4I7/ELEcTION7/7G7 "
    "BFLNOOR/EOSSSS 442/356 0 -lex CSW21",
};

static const char *const peg_fixture_files[] = {
    "notes/peg_positions/random_1peg.txt",
    "notes/peg_positions/random_2peg.txt",
    "notes/peg_positions/random_3peg.txt",
    "notes/peg_positions/random_4peg.txt",
};

static uint64_t next_random(uint64_t *state) {
  *state += 0x9E3779B97F4A7C15ULL;
  uint64_t value = *state;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

// Checks the player on turn of `game`.
static void check_player_on_turn(const Game *game, MoveList *all_small,
                                 MoveList *best_small, BestSmallStats *stats) {
  const int player_idx = game_get_player_on_turn_index(game);
  const Rack *rack = player_get_rack(game_get_player(game, player_idx));
  const int rack_size = rack_get_total_letters(rack);
  if (rack_size == 0) {
    return;
  }
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - player_idx));
  const Equity bonuses[BONUS_KINDS] = {
      0,
      calculate_end_rack_points(opponent_rack, game_get_ld(game)),
      // An arbitrary bonus that lets whole-rack plays tie plays that do not
      // use the rack, which the real bonus does only by accident.
      int_to_equity(9),
  };

  const MoveGenArgs args = {
      .game = game,
      .move_list = all_small,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  const int all_count = move_list_get_count(all_small);
  assert(all_count < ALL_SMALL_CAPACITY);
  stats->positions++;

  const LetterDistribution *ld = game_get_ld(game);
  uint64_t unweighted_tiny_moves[BONUS_KINDS] = {0};
  for (int check_idx = 0; check_idx < FACE_WEIGHT_KINDS * BONUS_KINDS;
       check_idx++) {
    const int face_weight = face_weights[check_idx / BONUS_KINDS];
    const int bonus_idx = check_idx % BONUS_KINDS;
    const Equity bonus = bonuses[bonus_idx];
    bool found_play = false;
    Equity best_value = 0;
    uint64_t best_tiny_move = 0;
    uint16_t best_score = 0;
    int ties_at_best = 0;
    for (int move_idx = 0; move_idx < all_count; move_idx++) {
      const SmallMove *small_move = all_small->small_moves[move_idx];
      if (small_move_is_pass(small_move)) {
        continue;
      }
      const Equity value =
          small_move_rank(small_move, ld, rack_size, bonus, face_weight);
      if (!found_play || value > best_value) {
        found_play = true;
        best_value = value;
        best_tiny_move = small_move->tiny_move;
        best_score = small_move_get_score(small_move);
        ties_at_best = 1;
      } else if (value == best_value) {
        ties_at_best++;
        if (small_move->tiny_move > best_tiny_move) {
          best_tiny_move = small_move->tiny_move;
          best_score = small_move_get_score(small_move);
        }
      }
    }

    MoveGenArgs best_args = args;
    best_args.move_list = best_small;
    best_args.move_record_type =
        face_weight == 0 ? MOVE_RECORD_BEST_SMALL : MOVE_RECORD_BEST_SMALL_FACE;
    best_args.best_small_face_weight = face_weight;
    best_args.best_small_out_bonus = bonus;
    best_args.best_small_canonical_ties = false;
    generate_moves(&best_args);
    assert(move_list_get_count(best_small) == 1);
    const SmallMove *first_found = best_small->small_moves[0];
    assert(small_move_is_pass(first_found) == !found_play);
    if (found_play) {
      assert(small_move_rank(first_found, ld, rack_size, bonus, face_weight) ==
             best_value);
    }

    best_args.best_small_canonical_ties = true;
    generate_moves(&best_args);
    assert(move_list_get_count(best_small) == 1);
    const SmallMove *result = best_small->small_moves[0];
    stats->checks++;
    if (!found_play) {
      assert(small_move_is_pass(result));
      stats->pass_results++;
      continue;
    }
    assert(!small_move_is_pass(result));
    assert(result->tiny_move == best_tiny_move);
    assert(small_move_get_score(result) == best_score);
    if (face_weight == 0) {
      unweighted_tiny_moves[bonus_idx] = result->tiny_move;
    } else if (result->tiny_move != unweighted_tiny_moves[bonus_idx]) {
      stats->face_weight_changes++;
    }
    if (ties_at_best >= 2) {
      stats->multiway_ties++;
    }
    if (small_move_get_tiles_played(result) == rack_size && bonus > 0) {
      stats->out_bonus_wins++;
    }
  }
}

static void check_both_players(Game *game, MoveList *all_small,
                               MoveList *best_small, BestSmallStats *stats) {
  const int original_player_idx = game_get_player_on_turn_index(game);
  for (int offset = 0; offset < 2; offset++) {
    game_set_player_on_turn_index(game, (original_player_idx + offset) % 2);
    check_player_on_turn(game, all_small, best_small, stats);
  }
  game_set_player_on_turn_index(game, original_player_idx);
}

static void check_self_play_games(const char *lexicon, MoveList *all_small,
                                  MoveList *best_small, BestSmallStats *stats) {
  char *command = get_formatted_string(
      "set -lex %s -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1",
      lexicon);
  Config *config = config_create_or_die(command);
  free(command);
  Game *game = config_game_create(config);
  MoveList *candidates = move_list_create(SELF_PLAY_CANDIDATE_MOVES);
  for (int game_idx = 0; game_idx < SELF_PLAY_GAMES_PER_LEXICON; game_idx++) {
    game_reset(game);
    const uint64_t seed = SELF_PLAY_BASE_SEED + (uint64_t)game_idx;
    game_seed(game, seed);
    draw_starting_racks(game);
    uint64_t choice_state = seed * 7919;
    for (int move_count = 0;
         move_count < SELF_PLAY_MAX_MOVES && !game_over(game); move_count++) {
      check_both_players(game, all_small, best_small, stats);
      const MoveGenArgs args = {
          .game = game,
          .move_list = candidates,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
      };
      generate_moves(&args);
      const int candidate_count = move_list_get_count(candidates);
      assert(candidate_count > 0);
      const int pick =
          (int)(next_random(&choice_state) % (uint64_t)candidate_count);
      play_move(move_list_get_move(candidates, pick), game, NULL);
    }
  }
  move_list_destroy(candidates);
  game_destroy(game);
  config_destroy(config);
}

// One best-play generation of the player on turn of `game`, through `cache`
// when it is not NULL, by the face ranking when `face_weight` is not 0.
static SmallMove best_small_play(const Game *game, LaneShadowCache *cache,
                                 MoveList *best_small, Equity bonus,
                                 int face_weight) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = best_small,
      .move_record_type = face_weight == 0 ? MOVE_RECORD_BEST_SMALL
                                           : MOVE_RECORD_BEST_SMALL_FACE,
      .move_sort_type = MOVE_SORT_SCORE,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .best_small_out_bonus = bonus,
      .best_small_face_weight = face_weight,
      .best_small_canonical_ties = true,
      .lane_shadow_cache = cache,
  };
  generate_moves(&args);
  assert(move_list_get_count(best_small) == 1);
  return *best_small->small_moves[0];
}

static void assert_same_small_play(const SmallMove *first,
                                   const SmallMove *second) {
  assert(first->tiny_move == second->tiny_move);
  assert(small_move_get_score(first) == small_move_get_score(second));
}

// Sets the bits of every square of board that no square field holds and the
// shadow does not read: the tile's owner, the flags byte's unused bits and
// the padding after that byte. A board allocated uninitialized leaves them
// holding whatever its memory held, so two boards with the same tiles can
// differ in them.
static void scribble_unread_square_bits(Board *board) {
  const size_t padding_start =
      offsetof(Square, bonus_square) + sizeof(BonusSquare) + 1;
  for (size_t square_idx = 0;
       square_idx < sizeof(board->squares) / sizeof(Square); square_idx++) {
    Square *square = &board->squares[square_idx];
    square->player_idx = 1;
    square->_pad = 0xF;
    unsigned char *bytes = (unsigned char *)square;
    for (size_t byte_idx = padding_start; byte_idx < sizeof(Square);
         byte_idx++) {
      bytes[byte_idx] = UINT8_MAX;
    }
  }
}

// The same positions with a word info table (`game`) and without one
// (`reference`): for the player on turn, without and with the out bonus and
// by each face weight, the play without a cache, through game's cache twice
// (recording its lanes, then finding them) and through the reference's cache.
// The rankings share the caches, which must keep their lanes and plays apart.
static void check_lane_shadow_cache_position(
    const Game *game, const Game *reference, LaneShadowCache *cache,
    LaneShadowCache *reference_cache, MoveList *best_small, int *checks) {
  const int player_idx = game_get_player_on_turn_index(game);
  if (rack_get_total_letters(
          player_get_rack(game_get_player(game, player_idx))) == 0) {
    return;
  }
  const Equity bonuses[] = {
      0, calculate_end_rack_points(
             player_get_rack(game_get_player(game, 1 - player_idx)),
             game_get_ld(game))};
  const size_t num_bonuses = sizeof(bonuses) / sizeof(Equity);
  for (size_t check_idx = 0; check_idx < FACE_WEIGHT_KINDS * num_bonuses;
       check_idx++) {
    const Equity bonus = bonuses[check_idx % num_bonuses];
    const int face_weight = face_weights[check_idx / num_bonuses];
    const SmallMove uncached =
        best_small_play(game, NULL, best_small, bonus, face_weight);
    for (int pass_idx = 0; pass_idx < 2; pass_idx++) {
      const SmallMove cached =
          best_small_play(game, cache, best_small, bonus, face_weight);
      assert_same_small_play(&cached, &uncached);
    }
    const SmallMove reference_play = best_small_play(
        reference, reference_cache, best_small, bonus, face_weight);
    assert_same_small_play(&reference_play, &uncached);
    (*checks)++;
  }
}

// With a word info table loaded, best-play generations use the lane shadow
// cache, which a loaded table once turned off, and record exactly the shadows
// they record without one: the small shadow they run reads no word info
// table, so the cache's keys hold all of its inputs. The table is built in
// memory, as witsweep builds its own, and the games' boards hold its block
// rows, which only the full shadow reads.
static void check_lane_shadow_cache_with_word_info_table(MoveList *best_small) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -wit false -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  PlayersData *players_data = config_get_players_data(config);
  assert(players_data_get_word_info_table(players_data, 0) == NULL);
  WordInfoTable *wit =
      make_word_info_table_from_kwg(players_data_get_kwg(players_data, 0), 1);
  // A game reads the table of its players' data when it is created.
  players_data_set_data(players_data, PLAYERS_DATA_TYPE_WIT, 0, wit);
  players_data_set_data(players_data, PLAYERS_DATA_TYPE_WIT, 1, wit);
  Game *game = config_game_create(config);
  players_data_set_data(players_data, PLAYERS_DATA_TYPE_WIT, 0, NULL);
  players_data_set_data(players_data, PLAYERS_DATA_TYPE_WIT, 1, NULL);
  Game *reference = config_game_create(config);
  assert(player_get_word_info_table(game_get_player(game, 0)) == wit);
  assert(player_get_word_info_table(game_get_player(reference, 0)) == NULL);
  LaneShadowCache *cache = lane_shadow_cache_create(
      LANE_SHADOW_TEST_INDEX_BITS, LANE_SHADOW_TEST_INDEX_BITS);
  LaneShadowCache *reference_cache = lane_shadow_cache_create(
      LANE_SHADOW_TEST_INDEX_BITS, LANE_SHADOW_TEST_INDEX_BITS);
  lane_shadow_cache_set_salt(cache, 1);
  lane_shadow_cache_set_salt(reference_cache, 1);
  MoveList *top_move = move_list_create(1);
  int checks = 0;
  int boards_with_rows = 0;
  for (int game_idx = 0; game_idx < LANE_SHADOW_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_reset(reference);
    // The caches must agree whatever those bits hold.
    scribble_unread_square_bits(game_get_board(reference));
    const uint64_t seed = SELF_PLAY_BASE_SEED + (uint64_t)game_idx;
    game_seed(game, seed);
    game_seed(reference, seed);
    draw_starting_racks(game);
    draw_starting_racks(reference);
    while (!game_over(game)) {
      boards_with_rows +=
          board_get_wit_cache_populated(game_get_board(game)) ? 1 : 0;
      const int on_turn_idx = game_get_player_on_turn_index(game);
      for (int offset = 0; offset < 2; offset++) {
        game_set_player_on_turn_index(game, (on_turn_idx + offset) % 2);
        game_set_player_on_turn_index(reference, (on_turn_idx + offset) % 2);
        check_lane_shadow_cache_position(game, reference, cache,
                                         reference_cache, best_small, &checks);
      }
      game_set_player_on_turn_index(game, on_turn_idx);
      game_set_player_on_turn_index(reference, on_turn_idx);
      const Move move = *get_top_equity_move(reference, top_move);
      play_move(&move, game, NULL);
      play_move(&move, reference, NULL);
    }
    assert(game_over(reference));
  }
  int recorded = 0;
  for (int entry_idx = 0; entry_idx < (1 << LANE_SHADOW_TEST_INDEX_BITS);
       entry_idx++) {
    const LaneShadowEntry *entry = &cache->entries[entry_idx];
    const LaneShadowEntry *reference_entry =
        &reference_cache->entries[entry_idx];
    assert(entry->key == reference_entry->key);
    assert(entry->count == reference_entry->count);
    for (int anchor_idx = 0; anchor_idx < entry->count; anchor_idx++) {
      assert(entry->cols[anchor_idx] == reference_entry->cols[anchor_idx]);
      assert(entry->last_anchor_cols[anchor_idx] ==
             reference_entry->last_anchor_cols[anchor_idx]);
      assert(entry->bounds[anchor_idx] == reference_entry->bounds[anchor_idx]);
    }
    recorded += entry->key != 0 ? 1 : 0;
  }
  // The anchors' plays, which the cache also holds: the small generator that
  // finds them reads no table either.
  int remembered = 0;
  for (int play_idx = 0; play_idx < (1 << LANE_SHADOW_TEST_INDEX_BITS);
       play_idx++) {
    const AnchorPlayEntry *play = &cache->anchor_plays[play_idx];
    const AnchorPlayEntry *reference_play =
        &reference_cache->anchor_plays[play_idx];
    assert(play->key == reference_play->key);
    assert(play->tiny_move == reference_play->tiny_move);
    assert(play->value == reference_play->value);
    assert(play->score == reference_play->score);
    assert(play->play_length == reference_play->play_length);
    assert(play->tiles_played == reference_play->tiles_played);
    remembered += play->key != 0 ? 1 : 0;
  }
  printf("best_small: %d generations through the lane shadow cache with a word "
         "info table, %d lanes recorded and %d anchor plays remembered, the "
         "same as without the table; %d boards held its block rows\n",
         checks, recorded, remembered, boards_with_rows);
  assert(recorded > 0);
  assert(remembered > 0);
  assert(boards_with_rows > 0);
  move_list_destroy(top_move);
  lane_shadow_cache_destroy(reference_cache);
  lane_shadow_cache_destroy(cache);
  game_destroy(reference);
  game_destroy(game);
  word_info_table_destroy(wit);
  config_destroy(config);
}

// generate_best_small_move_with_pool must find the play generate_moves finds.
// Over self-play games, each position's pool is the tiles of both racks, and
// the racks checked are the rack on turn and others of its size drawn from
// the pool, which share one cache as the draws of an out-play check do: each
// is generated without a cache, twice with the pool, the second time over
// what the first remembered, and once with a wider pool. The last racks hold
// blanks, which the pool's own tiles rarely give, and use the wider pool
// throughout.
static void check_pool_generations(const char *lexicon, bool use_wmp,
                                   MoveList *best_small) {
  char *config_str = get_formatted_string(
      "set -lex %s -wmp %s -s1 equity -s2 equity -r1 all -r2 all -numplays 1",
      lexicon, use_wmp ? "true" : "false");
  Config *config = config_create_or_die(config_str);
  free(config_str);
  Game *game = config_game_create(config);
  const int dist_size = ld_get_size(game_get_ld(game));
  LaneShadowCache *cache = lane_shadow_cache_create(
      LANE_SHADOW_TEST_INDEX_BITS, LANE_SHADOW_TEST_INDEX_BITS);
  lane_shadow_cache_set_salt(cache, 1);
  MoveList *top_move = move_list_create(1);
  const Equity bonus = int_to_equity(POOL_TEST_OUT_BONUS);
  uint64_t random_state = SELF_PLAY_BASE_SEED;
  int checks = 0;
  int whole_rack_plays = 0;
  int blank_whole_rack_plays[POOL_TEST_BLANK_RACKS + 1] = {0};
  int passes = 0;
  for (int game_idx = 0; game_idx < POOL_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, SELF_PLAY_BASE_SEED + (uint64_t)game_idx);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int on_turn_idx = game_get_player_on_turn_index(game);
      Rack *rack = player_get_rack(game_get_player(game, on_turn_idx));
      const Rack *opp_rack =
          player_get_rack(game_get_player(game, 1 - on_turn_idx));
      Rack original;
      rack_copy(&original, rack);
      const int rack_size = rack_get_total_letters(rack);
      Rack pool;
      rack_copy(&pool, rack);
      MachineLetter tiles[2 * RACK_SIZE];
      int num_tiles = 0;
      for (int ml = 0; ml < dist_size; ml++) {
        rack_add_letters(&pool, (MachineLetter)ml,
                         rack_get_letter(opp_rack, (MachineLetter)ml));
        for (int copy_idx = 0;
             copy_idx < rack_get_letter(&pool, (MachineLetter)ml); copy_idx++) {
          tiles[num_tiles++] = (MachineLetter)ml;
        }
      }
      // With two more blanks the pool makes plays of the whole rack from more
      // sets of tiles than a pool anchor keeps.
      Rack wide_pool;
      rack_copy(&wide_pool, &pool);
      rack_add_letters(&wide_pool, BLANK_MACHINE_LETTER, 2);
      const MoveGenArgs args = {
          .game = game,
          .move_list = best_small,
          .move_record_type = MOVE_RECORD_BEST_SMALL,
          .move_sort_type = MOVE_SORT_SCORE,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .best_small_out_bonus = bonus,
          .best_small_canonical_ties = true,
          .lane_shadow_cache = cache,
      };
      assert(num_tiles >= rack_size);
      for (int rack_idx = 0; rack_idx < POOL_TEST_RACKS; rack_idx++) {
        int rack_blanks =
            rack_idx - (POOL_TEST_RACKS - POOL_TEST_BLANK_RACKS) + 1;
        if (rack_blanks < 0 || rack_blanks > rack_size) {
          rack_blanks = 0;
        }
        if (rack_idx > 0) {
          rack_set_dist_size_and_reset(rack, dist_size);
          rack_add_letters(rack, BLANK_MACHINE_LETTER, rack_blanks);
          for (int pick_idx = 0; pick_idx < rack_size - rack_blanks;
               pick_idx++) {
            const int swap_idx =
                pick_idx + (int)(next_random(&random_state) %
                                 (uint64_t)(num_tiles - pick_idx));
            const MachineLetter picked = tiles[swap_idx];
            tiles[swap_idx] = tiles[pick_idx];
            tiles[pick_idx] = picked;
            rack_add_letter(rack, picked);
          }
        }
        const SmallMove expected =
            best_small_play(game, NULL, best_small, bonus, 0);
        for (int pass_idx = 0; pass_idx < 3; pass_idx++) {
          generate_best_small_move_with_pool(
              &args, pass_idx < 2 && rack_blanks == 0 ? &pool : &wide_pool);
          assert(move_list_get_count(best_small) == 1);
          const SmallMove *found = best_small->small_moves[0];
          assert_same_small_play(&expected, found);
          assert(small_move_get_tiles_played(&expected) ==
                 small_move_get_tiles_played(found));
          checks++;
        }
        if (small_move_is_pass(&expected)) {
          passes++;
        } else if (small_move_get_tiles_played(&expected) == rack_size) {
          whole_rack_plays++;
          blank_whole_rack_plays[rack_blanks]++;
        }
      }
      rack_copy(rack, &original);
      const Move move = *get_top_equity_move(game, top_move);
      play_move(&move, game, NULL);
    }
  }
  printf("best_small: %d generations with a pool (%s, %s the word map) find "
         "the plays generate_moves finds: %d of the whole rack, %d passes\n",
         checks, lexicon, use_wmp ? "with" : "without", whole_rack_plays,
         passes);
  assert(checks > 3 * POOL_TEST_RACKS * 15 * POOL_TEST_GAMES);
  assert(whole_rack_plays > 0);
  for (int blanks = 0; blanks <= POOL_TEST_BLANK_RACKS; blanks++) {
    assert(blank_whole_rack_plays[blanks] > 0);
  }
  move_list_destroy(top_move);
  lane_shadow_cache_destroy(cache);
  game_destroy(game);
  config_destroy(config);
}

static void check_cgp(Config *config, const char *cgp, MoveList *all_small,
                      MoveList *best_small, BestSmallStats *stats) {
  char *command = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, command);
  free(command);
  check_both_players(config_get_game(config), all_small, best_small, stats);
}

// The plays a lane shadow cache remembers for anchors (see AnchorPlayEntry in
// lane_shadow_cache.h) are keyed by the lane's squares, and also by which lane
// it is, by the anchor's column and by how ties are broken. Each of these
// boards holds the same squares in two lanes, where a key without the lane
// answers one lane with the other's play.
typedef struct TwinLaneBoard {
  const char *board;
  // The two lanes with the same squares: rows, or columns when vertical.
  bool vertical;
  int first_lane;
  int second_lane;
} TwinLaneBoard;

static const TwinLaneBoard twin_lane_boards[] = {
    // The columns of LEVEL's two Ls.
    {"15/15/15/15/15/15/15/5LEVEL5/15/15/15/15/15/15/15", true, 5, 9},
    // The rows of ROTATOR's two Rs.
    {"15/15/15/15/7R7/7O7/7T7/7A7/7T7/7O7/7R7/15/15/15/15", false, 4, 10},
    // The rows of CIVIC's two Cs. The rows of its two Is are not the same:
    // above LEVEL and below it, their cross sets differ.
    {"15/15/15/15/15/7C7/7I7/5LEVEL5/7I7/7C7/15/15/15/15/15", false, 5, 9},
};

typedef struct LaneIncumbentStats {
  int checks;
  int incumbents_beaten;
  // Positions where a face weight changes the best play of all the lanes.
  int face_weight_changes;
} LaneIncumbentStats;

typedef struct TwinLaneStats {
  int checks;
  int twin_lane_plays;
  int tie_rule_changes;
} TwinLaneStats;

// Takes the rack's tile number tile_idx, in letter order, off it.
static void rack_take_tile_at(Rack *rack, int tile_idx) {
  MachineLetter ml = 0;
  int tiles_before = (int)rack_get_letter(rack, ml);
  while (tiles_before <= tile_idx) {
    ml++;
    tiles_before += (int)rack_get_letter(rack, ml);
  }
  rack_take_letter(rack, ml);
}

// The best play of the player on turn, through cache unless it is NULL: on
// the whole board when lane is negative, and otherwise in that lane alone
// (rows and then columns). A pass when there is no play.
static SmallMove best_small_through_cache(const Game *game,
                                          LaneShadowCache *cache,
                                          MoveList *best_small, int lane,
                                          bool canonical_ties) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = best_small,
      .move_record_type = MOVE_RECORD_BEST_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .best_small_out_bonus = 0,
      .best_small_canonical_ties = canonical_ties,
      .lane_shadow_cache = cache,
  };
  if (lane < 0) {
    generate_moves(&args);
    assert(move_list_get_count(best_small) == 1);
    return *best_small->small_moves[0];
  }
  SmallMove play;
  small_move_set_as_pass(&play);
  if (generate_best_small_move_in_lanes(&args, UINT64_C(1) << lane, NULL, 0)) {
    play = *best_small->small_moves[0];
  }
  return play;
}

static void assert_small_plays_equal(const SmallMove *first,
                                     const SmallMove *second) {
  assert(first->tiny_move == second->tiny_move);
  assert(small_move_get_score(first) == small_move_get_score(second));
  assert(small_move_get_tiles_played(first) ==
         small_move_get_tiles_played(second));
  assert(small_move_get_play_length(first) ==
         small_move_get_play_length(second));
}

// On the board and in every lane, under both tie rules, the play found
// through the cache must be the play found without it. The second pass asks
// again once the cache holds every anchor under both rules.
static void check_twin_lane_position(const Game *game,
                                     const TwinLaneBoard *twin_lanes,
                                     LaneShadowCache *cache,
                                     MoveList *best_small,
                                     TwinLaneStats *stats) {
  for (int pass_idx = 0; pass_idx < TWIN_LANE_PASSES; pass_idx++) {
    SmallMove lane_plays[2][2 * BOARD_DIM];
    for (int canonical_ties = 0; canonical_ties < 2; canonical_ties++) {
      for (int lane = -1; lane < 2 * BOARD_DIM; lane++) {
        const SmallMove expected = best_small_through_cache(
            game, NULL, best_small, lane, canonical_ties != 0);
        const SmallMove play = best_small_through_cache(
            game, cache, best_small, lane, canonical_ties != 0);
        assert_small_plays_equal(&play, &expected);
        stats->checks++;
        if (lane >= 0) {
          lane_plays[canonical_ties][lane] = expected;
        }
      }
    }
    if (pass_idx != 0) {
      continue;
    }
    for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
      if (lane_plays[0][lane].tiny_move != lane_plays[1][lane].tiny_move) {
        stats->tie_rule_changes++;
      }
    }
    const int first_lane =
        (twin_lanes->vertical ? BOARD_DIM : 0) + twin_lanes->first_lane;
    const int second_lane =
        (twin_lanes->vertical ? BOARD_DIM : 0) + twin_lanes->second_lane;
    const SmallMove *first_play = &lane_plays[1][first_lane];
    const SmallMove *second_play = &lane_plays[1][second_lane];
    // The same squares hold the same play, in another place.
    assert(small_move_get_score(first_play) ==
           small_move_get_score(second_play));
    assert(small_move_is_pass(first_play) == small_move_is_pass(second_play));
    if (!small_move_is_pass(first_play)) {
      assert(first_play->tiny_move != second_play->tiny_move);
      stats->twin_lane_plays++;
    }
  }
}

// A high incumbent can leave a lane with no recorded play. Asking for the
// lane again with a weaker incumbent must reopen it, even at an equal bound
// where only the canonical tie decides the winner.
static void check_cached_lane_cutoff(Config *config, LaneShadowCache *cache,
                                     MoveList *best_small) {
  const TwinLaneBoard *twins = &twin_lane_boards[0];
  char *command = get_formatted_string("cgp %s A/ 0/0 0", twins->board);
  load_and_exec_config_or_die(config, command);
  free(command);
  const Game *game = config_get_game(config);
  const int lanes[] = {BOARD_DIM + twins->first_lane,
                       BOARD_DIM + twins->second_lane};
  const SmallMove plays[] = {
      best_small_through_cache(game, NULL, best_small, lanes[0], true),
      best_small_through_cache(game, NULL, best_small, lanes[1], true)};
  assert(!small_move_is_pass(&plays[0]));
  assert(small_move_get_score(&plays[0]) == small_move_get_score(&plays[1]));
  assert(plays[0].tiny_move != plays[1].tiny_move);
  const int winner = plays[1].tiny_move > plays[0].tiny_move;
  const SmallMove *incumbent = &plays[1 - winner];
  const int winning_lane = lanes[winner];
  assert(winning_lane >= 0 && winning_lane < 2 * BOARD_DIM);
  const uint64_t mask = UINT64_C(1) << winning_lane;
  const MoveGenArgs args = {
      .game = game,
      .move_list = best_small,
      .move_record_type = MOVE_RECORD_BEST_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
      .best_small_canonical_ties = true,
      .lane_shadow_cache = cache,
  };
  assert(generate_best_small_move_in_lanes(&args, mask, incumbent,
                                           EQUITY_MAX_VALUE - 1));
  assert_small_plays_equal(best_small->small_moves[0], incumbent);
  assert(generate_best_small_move_in_lanes(
      &args, mask, incumbent, int_to_equity(small_move_get_score(incumbent))));
  assert_small_plays_equal(best_small->small_moves[0], &plays[winner]);
  assert(generate_best_small_move_in_lanes(&args, mask, NULL, 0));
  assert_small_plays_equal(best_small->small_moves[0], &plays[winner]);
}

static void check_twin_lane_boards(MoveList *best_small) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  // One cache for every board and rack, as an endgame has one for every
  // position of a solve.
  LaneShadowCache *cache = lane_shadow_cache_create(TWIN_LANE_CACHE_INDEX_BITS,
                                                    TWIN_LANE_CACHE_INDEX_BITS);
  lane_shadow_cache_set_salt(cache, 1);
  check_cached_lane_cutoff(config, cache, best_small);
  TwinLaneStats stats = {0};
  for (size_t board_idx = 0;
       board_idx < sizeof(twin_lane_boards) / sizeof(TwinLaneBoard);
       board_idx++) {
    const TwinLaneBoard *twin_lanes = &twin_lane_boards[board_idx];
    char *command = get_formatted_string("cgp %s / 0/0 0", twin_lanes->board);
    load_and_exec_config_or_die(config, command);
    free(command);
    Game *game = config_get_game(config);
    uint64_t random_state = TWIN_LANE_SEED + (uint64_t)board_idx;
    game_seed(game, random_state);
    const int player_idx = game_get_player_on_turn_index(game);
    Rack *rack = player_get_rack(game_get_player(game, player_idx));
    const int twin_lane_plays_before = stats.twin_lane_plays;
    for (int rack_idx = 0; rack_idx < TWIN_LANE_FULL_RACKS_PER_BOARD;
         rack_idx++) {
      // A full rack, and then what is left of it as its tiles go one by one:
      // an endgame's racks are subsets of one rack.
      set_random_rack(game, player_idx, NULL);
      Rack full_rack;
      rack_copy(&full_rack, rack);
      while (!rack_is_empty(rack)) {
        check_twin_lane_position(game, twin_lanes, cache, best_small, &stats);
        rack_take_tile_at(rack, (int)(next_random(&random_state) %
                                      (uint64_t)rack_get_total_letters(rack)));
      }
      // The next rack's draw returns this one to the bag, whole.
      rack_copy(rack, &full_rack);
    }
    assert(stats.twin_lane_plays > twin_lane_plays_before);
  }
  printf("best_small: %d checks through a lane shadow cache, %d racks with a "
         "play in both of two lanes with the same squares, %d lanes whose "
         "best play the tie rule changes\n",
         stats.checks, stats.twin_lane_plays, stats.tie_rule_changes);
  assert(stats.tie_rule_changes > 0);
  lane_shadow_cache_destroy(cache);
  config_destroy(config);
}

// A lane's remembered best play (see AnchorPlayEntry in lane_shadow_cache.h)
// answers only against a best so far above every bound its search left out,
// and a search that left an anchor out at a bound equal to the lane's best
// has not settled a tie there. So each lane's best play in turn, the highest
// ranked first, is the incumbent for a generation over the lanes whose turn
// is still to come, none of which ranks higher: the early generations leave
// most anchors out, the later, weaker incumbents find entries that must be
// searched further, and only a tie beats an incumbent. Each generation
// through the cache must give the play it gives without one. Plays rank by
// small_move_rank with `face_weight` and no out bonus; the caller runs every
// face weight through the same cache, which must keep their lanes apart.
// Returns the best play of all the lanes, or a pass.
static SmallMove check_lane_incumbents_position(const Game *game,
                                                LaneShadowCache *cache,
                                                MoveList *best_small,
                                                int face_weight,
                                                LaneIncumbentStats *stats) {
  const LetterDistribution *ld = game_get_ld(game);
  const int rack_size = rack_get_total_letters(player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game))));
  MoveGenArgs args = {
      .game = game,
      .move_list = best_small,
      .move_record_type = face_weight == 0 ? MOVE_RECORD_BEST_SMALL
                                           : MOVE_RECORD_BEST_SMALL_FACE,
      .move_sort_type = MOVE_SORT_SCORE,
      .target_equity = EQUITY_MAX_VALUE,
      .best_small_face_weight = face_weight,
      .best_small_canonical_ties = true,
  };
  SmallMove lane_plays[2 * BOARD_DIM];
  Equity lane_ranks[2 * BOARD_DIM] = {0};
  uint64_t lanes_with_plays = 0;
  for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
    if (generate_best_small_move_in_lanes(&args, UINT64_C(1) << lane, NULL,
                                          0)) {
      lane_plays[lane] = *best_small->small_moves[0];
      lane_ranks[lane] =
          small_move_rank(&lane_plays[lane], ld, rack_size, 0, face_weight);
      lanes_with_plays |= UINT64_C(1) << lane;
    }
  }
  const uint64_t all_lanes = lanes_with_plays;
  SmallMove best_of_all;
  small_move_set_as_pass(&best_of_all);
  // One more round than there are lanes with plays: the last has no
  // incumbent, and covers every one of them.
  bool has_incumbent = true;
  while (has_incumbent) {
    int incumbent_lane = -1;
    for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
      if (((lanes_with_plays >> lane) & 1) != 0 &&
          (incumbent_lane < 0 ||
           lane_ranks[lane] > lane_ranks[incumbent_lane])) {
        incumbent_lane = lane;
      }
    }
    has_incumbent = incumbent_lane >= 0;
    const SmallMove *incumbent = NULL;
    Equity incumbent_value = 0;
    uint64_t lane_mask = all_lanes;
    if (has_incumbent) {
      incumbent = &lane_plays[incumbent_lane];
      incumbent_value = lane_ranks[incumbent_lane];
      lanes_with_plays &= ~(UINT64_C(1) << incumbent_lane);
      lane_mask = lanes_with_plays;
    }
    args.lane_shadow_cache = NULL;
    const bool expected_found = generate_best_small_move_in_lanes(
        &args, lane_mask, incumbent, incumbent_value);
    SmallMove expected;
    small_move_set_as_pass(&expected);
    if (expected_found) {
      expected = *best_small->small_moves[0];
    }
    args.lane_shadow_cache = cache;
    const bool found = generate_best_small_move_in_lanes(
        &args, lane_mask, incumbent, incumbent_value);
    assert(found == expected_found);
    if (found) {
      assert_small_plays_equal(best_small->small_moves[0], &expected);
      if (has_incumbent &&
          best_small->small_moves[0]->tiny_move != incumbent->tiny_move) {
        stats->incumbents_beaten++;
      }
      if (!has_incumbent) {
        best_of_all = *best_small->small_moves[0];
      }
    }
    stats->checks++;
  }
  return best_of_all;
}

static void check_lane_incumbents(MoveList *best_small) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  // One cache for every position, as an endgame has one for a solve.
  LaneShadowCache *cache = lane_shadow_cache_create(
      LANE_SHADOW_TEST_INDEX_BITS, LANE_SHADOW_TEST_INDEX_BITS);
  lane_shadow_cache_set_salt(cache, 1);
  MoveList *candidates = move_list_create(SELF_PLAY_CANDIDATE_MOVES);
  LaneIncumbentStats stats = {0};
  for (int game_idx = 0; game_idx < LANE_SHADOW_TEST_GAMES; game_idx++) {
    game_reset(game);
    const uint64_t seed = SELF_PLAY_BASE_SEED + (uint64_t)game_idx;
    game_seed(game, seed);
    draw_starting_racks(game);
    uint64_t choice_state = seed * 7919;
    for (int move_count = 0;
         move_count < SELF_PLAY_MAX_MOVES && !game_over(game); move_count++) {
      const int on_turn_idx = game_get_player_on_turn_index(game);
      for (int offset = 0; offset < 2; offset++) {
        game_set_player_on_turn_index(game, (on_turn_idx + offset) % 2);
        SmallMove unweighted;
        small_move_set_as_pass(&unweighted);
        for (int weight_idx = 0; weight_idx < FACE_WEIGHT_KINDS; weight_idx++) {
          const SmallMove best = check_lane_incumbents_position(
              game, cache, best_small, face_weights[weight_idx], &stats);
          if (weight_idx == 0) {
            unweighted = best;
          } else if (best.tiny_move != unweighted.tiny_move) {
            stats.face_weight_changes++;
          }
        }
      }
      game_set_player_on_turn_index(game, on_turn_idx);
      const MoveGenArgs args = {
          .game = game,
          .move_list = candidates,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
      };
      generate_moves(&args);
      const int candidate_count = move_list_get_count(candidates);
      assert(candidate_count > 0);
      const int pick =
          (int)(next_random(&choice_state) % (uint64_t)candidate_count);
      play_move(move_list_get_move(candidates, pick), game, NULL);
    }
  }
  printf("best_small: %d generations in lanes against an incumbent through a "
         "lane shadow cache, %d that beat it on a tie, %d best plays of all "
         "the lanes that a face weight changes\n",
         stats.checks, stats.incumbents_beaten, stats.face_weight_changes);
  assert(stats.incumbents_beaten > 0);
  assert(stats.face_weight_changes > 0);
  move_list_destroy(candidates);
  lane_shadow_cache_destroy(cache);
  game_destroy(game);
  config_destroy(config);
}

void test_best_small(void) {
  BestSmallStats stats = {0};
  MoveList *all_small = move_list_create_small(ALL_SMALL_CAPACITY);
  MoveList *best_small = move_list_create_small(1);

  check_twin_lane_boards(best_small);
  check_lane_incumbents(best_small);
  check_self_play_games("CSW21", all_small, best_small, &stats);
  check_self_play_games("NWL20", all_small, best_small, &stats);
  const int self_play_positions = stats.positions;

  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  for (size_t cgp_idx = 0; cgp_idx < sizeof(endgame_cgps) / sizeof(char *);
       cgp_idx++) {
    check_cgp(config, endgame_cgps[cgp_idx], all_small, best_small, &stats);
  }
  const int endgame_positions = stats.positions - self_play_positions;

  for (size_t file_idx = 0;
       file_idx < sizeof(peg_fixture_files) / sizeof(char *); file_idx++) {
    char *contents = get_string_from_file_or_die(peg_fixture_files[file_idx]);
    StringSplitter *lines = split_string_by_newline(contents, true);
    for (int line_idx = 0;
         line_idx < string_splitter_get_number_of_items(lines); line_idx++) {
      check_cgp(config, string_splitter_get_item(lines, line_idx), all_small,
                best_small, &stats);
    }
    string_splitter_destroy(lines);
    free(contents);
  }
  const int peg_positions =
      stats.positions - self_play_positions - endgame_positions;

  printf("best_small: %d positions (%d self-play, %d endgame cgp, %d peg), "
         "%d checks, %d with a multi-way tie at the maximum, %d won by the "
         "out bonus, %d passes, %d where a face weight changes the play\n",
         stats.positions, self_play_positions, endgame_positions, peg_positions,
         stats.checks, stats.multiway_ties, stats.out_bonus_wins,
         stats.pass_results, stats.face_weight_changes);
  assert(stats.multiway_ties > 0);
  assert(stats.face_weight_changes > 0);

  check_lane_shadow_cache_with_word_info_table(best_small);
  check_pool_generations("CSW21", true, best_small);
  check_pool_generations("CSW21", false, best_small);

  config_destroy(config);
  small_move_list_destroy(best_small);
  small_move_list_destroy(all_small);
}
