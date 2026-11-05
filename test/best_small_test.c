#include "best_small_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/lane_shadow_cache.h"
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
// still be a play of the best rank.

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
  SELF_PLAY_BASE_SEED = 1000,
  // The lane shadow cache check: its self-play games, and its caches' 2^16
  // lanes (4 MB) and 2^16 anchor plays (2 MB) each, more than those games
  // record.
  LANE_SHADOW_TEST_GAMES = 4,
  LANE_SHADOW_TEST_INDEX_BITS = 16,
};

typedef struct BestSmallStats {
  int positions;
  int checks;
  int multiway_ties;
  int pass_results;
  int out_bonus_wins;
} BestSmallStats;

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

  for (int bonus_idx = 0; bonus_idx < BONUS_KINDS; bonus_idx++) {
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
      Equity value = int_to_equity(small_move_get_score(small_move));
      if (small_move_get_tiles_played(small_move) == rack_size) {
        value += bonus;
      }
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
    best_args.move_record_type = MOVE_RECORD_BEST_SMALL;
    best_args.best_small_out_bonus = bonus;
    best_args.best_small_canonical_ties = false;
    generate_moves(&best_args);
    assert(move_list_get_count(best_small) == 1);
    const SmallMove *first_found = best_small->small_moves[0];
    assert(small_move_is_pass(first_found) == !found_play);
    if (found_play) {
      Equity first_found_value =
          int_to_equity(small_move_get_score(first_found));
      if (small_move_get_tiles_played(first_found) == rack_size) {
        first_found_value += bonus;
      }
      assert(first_found_value == best_value);
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
// when it is not NULL.
static SmallMove best_small_play(const Game *game, LaneShadowCache *cache,
                                 MoveList *best_small, Equity bonus) {
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
// (`reference`): for the player on turn, without and with the out bonus, the
// play without a cache, through game's cache twice (recording its lanes, then
// finding them) and through the reference's cache.
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
  for (size_t bonus_idx = 0; bonus_idx < sizeof(bonuses) / sizeof(Equity);
       bonus_idx++) {
    const Equity bonus = bonuses[bonus_idx];
    const SmallMove uncached = best_small_play(game, NULL, best_small, bonus);
    for (int pass_idx = 0; pass_idx < 2; pass_idx++) {
      const SmallMove cached = best_small_play(game, cache, best_small, bonus);
      assert_same_small_play(&cached, &uncached);
    }
    const SmallMove reference_play =
        best_small_play(reference, reference_cache, best_small, bonus);
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

static void check_twin_lane_boards(MoveList *best_small) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  // One cache for every board and rack, as an endgame has one for every
  // position of a solve.
  LaneShadowCache *cache = lane_shadow_cache_create(TWIN_LANE_CACHE_INDEX_BITS,
                                                    TWIN_LANE_CACHE_INDEX_BITS);
  lane_shadow_cache_set_salt(cache, 1);
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

void test_best_small(void) {
  BestSmallStats stats = {0};
  MoveList *all_small = move_list_create_small(ALL_SMALL_CAPACITY);
  MoveList *best_small = move_list_create_small(1);

  check_twin_lane_boards(best_small);
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
         "out bonus, %d passes\n",
         stats.positions, self_play_positions, endgame_positions, peg_positions,
         stats.checks, stats.multiway_ties, stats.out_bonus_wins,
         stats.pass_results);
  assert(stats.multiway_ties > 0);

  check_lane_shadow_cache_with_word_info_table(best_small);

  config_destroy(config);
  small_move_list_destroy(best_small);
  small_move_list_destroy(all_small);
}
