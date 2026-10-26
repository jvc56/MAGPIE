#include "best_small_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
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
  ALL_SMALL_CAPACITY = 250000,
  SELF_PLAY_GAMES_PER_LEXICON = 6,
  SELF_PLAY_MAX_MOVES = 120,
  SELF_PLAY_CANDIDATE_MOVES = 6,
  BONUS_KINDS = 3,
  SELF_PLAY_BASE_SEED = 1000,
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

static void check_cgp(Config *config, const char *cgp, MoveList *all_small,
                      MoveList *best_small, BestSmallStats *stats) {
  char *command = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, command);
  free(command);
  check_both_players(config_get_game(config), all_small, best_small, stats);
}

void test_best_small(void) {
  BestSmallStats stats = {0};
  MoveList *all_small = move_list_create_small(ALL_SMALL_CAPACITY);
  MoveList *best_small = move_list_create_small(1);

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

  config_destroy(config);
  small_move_list_destroy(best_small);
  small_move_list_destroy(all_small);
}
