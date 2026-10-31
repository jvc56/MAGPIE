#include "census_test.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/census.h"
#include "../src/util/io_util.h"
#include "../src/util/math_util.h"
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

void test_census(void) {
  test_lattice_ranks_round_trip();
  test_add_table();
  test_apportion_matches_naive();
  test_blank_variants_match_naive();
}
