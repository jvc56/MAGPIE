#include "census_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/census.h"
#include "../src/impl/census_play_scores.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/math_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static CensusLattice *create_lattice_or_die(int num_letters, int rack_size) {
  ErrorStack *error_stack = error_stack_create();
  CensusLattice *lattice =
      census_lattice_create(num_letters, rack_size, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return lattice;
}

static int tally_size(const uint8_t *tally, int num_letters) {
  int size = 0;
  for (int ml = 0; ml < num_letters; ml++) {
    size += tally[ml];
  }
  return size;
}

static void test_lattice_ranks_round_trip(void) {
  for (int num_letters = 1; num_letters <= 6; num_letters++) {
    for (int rack_size = 1; rack_size <= 5; rack_size++) {
      CensusLattice *lattice = create_lattice_or_die(num_letters, rack_size);
      const uint32_t size = census_lattice_get_size(lattice);
      // C(num_letters + rack_size, rack_size) multisets of 0 to rack_size
      assert(size == choose(num_letters + rack_size, rack_size));
      uint8_t tally[CENSUS_MAX_LETTERS];
      int previous_size = 0;
      for (uint32_t index = 0; index < size; index++) {
        census_lattice_unrank(lattice, index, tally);
        const int multiset_size = tally_size(tally, num_letters);
        // ordered by size first
        assert(multiset_size >= previous_size);
        assert(multiset_size <= rack_size);
        previous_size = multiset_size;
        assert(index >= census_lattice_get_size_start(lattice, multiset_size));
        assert(index <
               census_lattice_get_size_start(lattice, multiset_size + 1));
        assert(census_lattice_rank(lattice, tally) == index);
      }
      census_lattice_destroy(lattice);
    }
  }
  // English: 27 letters and 7 tiles
  CensusLattice *lattice = create_lattice_or_die(27, 7);
  assert(census_lattice_get_size(lattice) == 5379616);
  uint8_t tally[CENSUS_MAX_LETTERS];
  for (uint32_t index = 0; index < census_lattice_get_size(lattice);
       index += 997) {
    census_lattice_unrank(lattice, index, tally);
    assert(census_lattice_rank(lattice, tally) == index);
  }
  memset(tally, 0, sizeof(tally));
  tally[3] = 8;
  assert(census_lattice_rank(lattice, tally) == CENSUS_UNRANKED);
  census_lattice_destroy(lattice);

  ErrorStack *error_stack = error_stack_create();
  assert(!census_lattice_create(65, 7, error_stack));
  assert(error_stack_top(error_stack) == ERROR_STATUS_CENSUS_INVALID_LATTICE);
  error_stack_destroy(error_stack);
}

static void test_add_table(void) {
  CensusLattice *lattice = create_lattice_or_die(5, 4);
  CensusAddTable *add_table = census_add_table_create(lattice, 3);
  uint8_t tally[CENSUS_MAX_LETTERS];
  for (uint32_t index = 0; index < census_lattice_get_full_rack_start(lattice);
       index++) {
    for (int ml = 0; ml < 5; ml++) {
      census_lattice_unrank(lattice, index, tally);
      tally[ml]++;
      assert(census_add_table_add(add_table, index, ml) ==
             census_lattice_rank(lattice, tally));
    }
  }
  census_add_table_destroy(add_table);
  census_lattice_destroy(lattice);
}

// The slow references

// The best play_scores[P] + leave[K] over every split of rack into P + K.
static Equity naive_best_equity(const CensusLattice *lattice,
                                const Equity *play_scores, const Equity *leave,
                                const uint8_t *rack) {
  const int num_letters = census_lattice_get_num_letters(lattice);
  const uint32_t size = census_lattice_get_size(lattice);
  uint8_t played[CENSUS_MAX_LETTERS];
  uint8_t kept[CENSUS_MAX_LETTERS];
  Equity best = CENSUS_UNPLAYABLE;
  for (uint32_t index = 0; index < size; index++) {
    census_lattice_unrank(lattice, index, played);
    bool within = true;
    for (int ml = 0; ml < num_letters; ml++) {
      if (played[ml] > rack[ml]) {
        within = false;
        break;
      }
      kept[ml] = (uint8_t)(rack[ml] - played[ml]);
    }
    if (!within) {
      continue;
    }
    const Equity value =
        play_scores[index] + leave[census_lattice_rank(lattice, kept)];
    if (value > best) {
      best = value;
    }
  }
  return best;
}

// Every full rack the pool can deal, weighted by its draw count, pushed into
// each of its subracks.
static void naive_apportion(const CensusLattice *lattice,
                            const Equity *play_scores, const Equity *leave,
                            const uint8_t *unseen, double *num, double *den) {
  const int num_letters = census_lattice_get_num_letters(lattice);
  const uint32_t size = census_lattice_get_size(lattice);
  uint8_t rack[CENSUS_MAX_LETTERS];
  uint8_t subrack[CENSUS_MAX_LETTERS];
  for (uint32_t rack_index = census_lattice_get_full_rack_start(lattice);
       rack_index < size; rack_index++) {
    census_lattice_unrank(lattice, rack_index, rack);
    double weight = 1.0;
    for (int ml = 0; ml < num_letters; ml++) {
      weight *= (double)choose(unseen[ml], rack[ml]);
    }
    if (weight == 0.0) {
      continue;
    }
    const double best =
        (double)naive_best_equity(lattice, play_scores, leave, rack);
    for (uint32_t subrack_index = 0; subrack_index < size; subrack_index++) {
      census_lattice_unrank(lattice, subrack_index, subrack);
      bool within = true;
      for (int ml = 0; ml < num_letters; ml++) {
        if (subrack[ml] > rack[ml]) {
          within = false;
          break;
        }
      }
      if (within) {
        num[subrack_index] += weight * best;
        den[subrack_index] += weight;
      }
    }
  }
}

static void test_apportion_matches_naive(void) {
  XoshiroPRNG *prng = prng_create(42);
  const int num_letters = 5;
  const int rack_size = 4;
  CensusLattice *lattice = create_lattice_or_die(num_letters, rack_size);
  CensusAddTable *add_table = census_add_table_create(lattice, 2);
  const uint32_t size = census_lattice_get_size(lattice);
  Equity *play_scores = malloc_or_die(sizeof(Equity) * size);
  Equity *leave = malloc_or_die(sizeof(Equity) * size);
  Equity *rack_best = malloc_or_die(sizeof(Equity) * size);
  Equity *leave_max_over_subsets = malloc_or_die(sizeof(Equity) * size);
  double *expected_num = malloc_or_die(sizeof(double) * size);
  double *expected_den = malloc_or_die(sizeof(double) * size);
  CensusSums *sums = malloc_or_die(sizeof(CensusSums) * size);
  for (int trial_idx = 0; trial_idx < 40; trial_idx++) {
    const bool null_leave = trial_idx % 4 == 0;
    uint8_t unseen[CENSUS_MAX_LETTERS];
    for (int ml = 0; ml < num_letters; ml++) {
      unseen[ml] = (uint8_t)prng_get_random_number(prng, 6);
    }
    for (uint32_t index = 0; index < size; index++) {
      // Small whole values keep every sum exact in a double, so each path
      // must match the reference exactly. Most played groups score nothing,
      // as most groups spell no word.
      play_scores[index] = prng_get_random_number(prng, 3) == 0
                               ? (Equity)prng_get_random_number(prng, 50)
                               : 0;
      leave[index] =
          null_leave ? 0 : (Equity)prng_get_random_number(prng, 41) - 20;
    }
    play_scores[0] = 0;
    leave[0] = 0;
    memset(expected_num, 0, sizeof(double) * size);
    memset(expected_den, 0, sizeof(double) * size);
    naive_apportion(lattice, play_scores, leave, unseen, expected_num,
                    expected_den);
    census_max_over_subsets(lattice, add_table, leave, leave_max_over_subsets);
    // push, superset sum, scatter, and scatter from a max over subsets made
    // beforehand
    for (int mode_idx = 0; mode_idx < 4; mode_idx++) {
      const CensusApportionArgs args = {
          .lattice = lattice,
          .add_table = add_table,
          .play_scores = play_scores,
          .leave = leave,
          .leave_max_over_subsets =
              mode_idx == 3 ? leave_max_over_subsets : NULL,
          .unseen_tally = unseen,
          .null_leave = null_leave,
          .superset_sum = mode_idx >= 1,
          .scatter = mode_idx >= 2};
      memset(sums, 0, sizeof(CensusSums) * size);
      census_apportion(&args, sums, rack_best);
      for (uint32_t index = 0; index < size; index++) {
        assert(sums[index].num == expected_num[index]);
        assert(sums[index].den == expected_den[index]);
      }
    }
  }
  free(sums);
  free(expected_den);
  free(expected_num);
  free(leave_max_over_subsets);
  free(rack_best);
  free(leave);
  free(play_scores);
  census_add_table_destroy(add_table);
  census_lattice_destroy(lattice);
  prng_destroy(prng);
}

// Every choice of which placed tiles are blanks, each letter keeping at most
// min(unseen, rack_size) real copies and the play at most num_blanks blanks.
static void naive_blank_variants(const CensusLattice *lattice,
                                 Equity *play_scores, Equity real_score,
                                 const CensusPlacedTile *placed,
                                 int number_of_placed,
                                 const uint8_t *unseen_tally, int num_blanks) {
  const int num_letters = census_lattice_get_num_letters(lattice);
  const int rack_size = census_lattice_get_rack_size(lattice);
  for (uint32_t blanked = 0; blanked < (1U << number_of_placed); blanked++) {
    uint8_t tally[CENSUS_MAX_LETTERS];
    memset(tally, 0, sizeof(tally));
    Equity value = real_score;
    for (int placed_idx = 0; placed_idx < number_of_placed; placed_idx++) {
      if (blanked & (1U << placed_idx)) {
        tally[BLANK_MACHINE_LETTER]++;
        value -= placed[placed_idx].blank_drop;
      } else {
        tally[placed[placed_idx].ml]++;
      }
    }
    bool possible = tally[BLANK_MACHINE_LETTER] <= num_blanks;
    for (int ml = 1; ml < num_letters; ml++) {
      const int real_available =
          unseen_tally[ml] < rack_size ? unseen_tally[ml] : rack_size;
      if (tally[ml] > real_available) {
        possible = false;
      }
    }
    if (!possible) {
      continue;
    }
    const uint32_t rank = census_lattice_rank(lattice, tally);
    if (value > play_scores[rank]) {
      play_scores[rank] = value;
    }
  }
}

static void test_blank_variants_match_naive(void) {
  XoshiroPRNG *prng = prng_create(7);
  const int num_letters = 5;
  const int rack_size = 5;
  CensusLattice *lattice = create_lattice_or_die(num_letters, rack_size);
  const uint32_t size = census_lattice_get_size(lattice);
  Equity *play_scores = calloc_or_die(size, sizeof(Equity));
  Equity *expected = calloc_or_die(size, sizeof(Equity));
  for (int trial_idx = 0; trial_idx < 2000; trial_idx++) {
    uint8_t unseen[CENSUS_MAX_LETTERS];
    for (int ml = 0; ml < num_letters; ml++) {
      unseen[ml] = (uint8_t)prng_get_random_number(prng, 4);
    }
    const int num_blanks = unseen[BLANK_MACHINE_LETTER];
    const int number_of_placed = 1 + (int)prng_get_random_number(prng, 5);
    CensusPlacedTile placed[CENSUS_MAX_LETTERS] = {{0}};
    int placed_of_letter[CENSUS_MAX_LETTERS] = {0};
    int blanks_needed = 0;
    for (int placed_idx = 0; placed_idx < number_of_placed; placed_idx++) {
      const uint8_t ml = (uint8_t)(1 + prng_get_random_number(prng, 4));
      placed[placed_idx] = (CensusPlacedTile){
          .ml = ml, .blank_drop = (Equity)prng_get_random_number(prng, 30)};
      placed_of_letter[ml]++;
      if (placed_of_letter[ml] > unseen[ml]) {
        blanks_needed++;
      }
    }
    // a play the generator could have made: real tiles first, blanks only
    // for the copies the pool lacks
    if (blanks_needed > num_blanks) {
      continue;
    }
    const Equity real_score = 100 + (Equity)prng_get_random_number(prng, 50);
    CensusPlacedTile placed_copy[CENSUS_MAX_LETTERS];
    memcpy(placed_copy, placed, sizeof(placed));
    census_record_blank_variants(lattice, play_scores, real_score, placed_copy,
                                 number_of_placed, unseen, num_blanks);
    naive_blank_variants(lattice, expected, real_score, placed,
                         number_of_placed, unseen, num_blanks);
    for (uint32_t index = 0; index < size; index++) {
      assert(play_scores[index] == expected[index]);
    }
  }
  free(expected);
  free(play_scores);
  census_lattice_destroy(lattice);
  prng_destroy(prng);
}

typedef struct PlacedBest {
  uint32_t rank;
  Equity score;
} PlacedBest;

// For random racks drawn from the board's unseen pool, play_scores[P] must be
// the best score the move generator finds for a play placing exactly P, for
// every P within the rack.
static void assert_play_scores_match_movegen(Game *game,
                                             const CensusLattice *lattice,
                                             Equity *play_scores,
                                             XoshiroPRNG *prng,
                                             int number_of_racks) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const uint32_t size = census_lattice_get_size(lattice);
  uint8_t unseen[CENSUS_MAX_LETTERS];
  census_get_unseen_tally(game, unseen);
  for (uint32_t index = 0; index < size; index++) {
    play_scores[index] = CENSUS_UNPLAYABLE;
  }
  const uint64_t number_of_plays =
      census_play_scores_build(game, lattice, unseen, play_scores);
  assert(number_of_plays > 0);

  MachineLetter pool[200];
  int pool_size = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    for (int copy_idx = 0; copy_idx < unseen[ml]; copy_idx++) {
      pool[pool_size++] = (MachineLetter)ml;
    }
  }
  assert(pool_size >= RACK_SIZE);
  const int player_index = game_get_player_on_turn_index(game);
  Rack *rack = player_get_rack(game_get_player(game, player_index));
  MoveList *move_list = move_list_create(100000);
  for (int rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
    // draw RACK_SIZE tiles from the pool without replacement
    for (int draw_idx = 0; draw_idx < RACK_SIZE; draw_idx++) {
      const int pick = draw_idx + (int)prng_get_random_number(
                                      prng, (uint64_t)(pool_size - draw_idx));
      const MachineLetter swap = pool[draw_idx];
      pool[draw_idx] = pool[pick];
      pool[pick] = swap;
    }
    uint8_t rack_tally[CENSUS_MAX_LETTERS];
    memset(rack_tally, 0, sizeof(rack_tally));
    rack_reset(rack);
    for (int draw_idx = 0; draw_idx < RACK_SIZE; draw_idx++) {
      rack_add_letter(rack, pool[draw_idx]);
      rack_tally[pool[draw_idx]]++;
    }
    const MoveGenArgs move_gen_args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&move_gen_args);
    // the best score by placed multiset; a rack has at most 2^RACK_SIZE
    PlacedBest best_by_placed[1 << RACK_SIZE];
    int number_of_placed = 0;
    for (int move_idx = 0; move_idx < move_list_get_count(move_list);
         move_idx++) {
      const Move *move = move_list_get_move(move_list, move_idx);
      if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      uint8_t placed_tally[CENSUS_MAX_LETTERS];
      memset(placed_tally, 0, sizeof(placed_tally));
      for (int tile_idx = 0; tile_idx < move_get_tiles_length(move);
           tile_idx++) {
        const MachineLetter tile = move_get_tile(move, tile_idx);
        if (tile == PLAYED_THROUGH_MARKER) {
          continue;
        }
        placed_tally[get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile]++;
      }
      const uint32_t rank = census_lattice_rank(lattice, placed_tally);
      int found_idx = 0;
      while (found_idx < number_of_placed &&
             best_by_placed[found_idx].rank != rank) {
        found_idx++;
      }
      if (found_idx == number_of_placed) {
        best_by_placed[number_of_placed++] =
            (PlacedBest){.rank = rank, .score = move_get_score(move)};
      } else if (move_get_score(move) > best_by_placed[found_idx].score) {
        best_by_placed[found_idx].score = move_get_score(move);
      }
    }
    // every nonempty sub-multiset of the rack, as a mixed-radix counter over
    // the rack's letters
    uint8_t subrack[CENSUS_MAX_LETTERS];
    memset(subrack, 0, sizeof(subrack));
    while (true) {
      int ml = 0;
      while (ml < ld_size && subrack[ml] == rack_tally[ml]) {
        subrack[ml] = 0;
        ml++;
      }
      if (ml == ld_size) {
        break;
      }
      subrack[ml]++;
      const uint32_t index = census_lattice_rank(lattice, subrack);
      Equity expected = CENSUS_UNPLAYABLE;
      for (int found_idx = 0; found_idx < number_of_placed; found_idx++) {
        if (best_by_placed[found_idx].rank == index) {
          expected = best_by_placed[found_idx].score;
        }
      }
      assert(play_scores[index] == expected);
    }
  }
  move_list_destroy(move_list);
  rack_reset(rack);
}

static void test_play_scores_match_movegen(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  Game *game = config_game_create(config);
  const int num_letters = ld_get_size(game_get_ld(game));
  CensusLattice *lattice = create_lattice_or_die(num_letters, RACK_SIZE);
  Equity *play_scores =
      malloc_or_die(sizeof(Equity) * census_lattice_get_size(lattice));
  XoshiroPRNG *prng = prng_create(11);
  const char *cgps[] = {DOUG_V_EMELY_CGP, VS_ANDY_CGP, VS_FRENTZ_CGP,
                        NOAH_VS_PETER_CGP, GUY_VS_BOT_CGP};
  for (size_t cgp_idx = 0; cgp_idx < sizeof(cgps) / sizeof(cgps[0]);
       cgp_idx++) {
    load_cgp_or_die(game, cgps[cgp_idx]);
    assert_play_scores_match_movegen(game, lattice, play_scores, prng, 40);
  }
  prng_destroy(prng);
  free(play_scores);
  census_lattice_destroy(lattice);
  game_destroy(game);
  config_destroy(config);
}

void test_census(void) {
  test_lattice_ranks_round_trip();
  test_add_table();
  test_apportion_matches_naive();
  test_blank_variants_match_naive();
  test_play_scores_match_movegen();
}
