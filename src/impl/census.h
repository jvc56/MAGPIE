#ifndef CENSUS_H
#define CENSUS_H

#include "../ent/equity.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// The census values every leave exactly on one board, then averages over
// many boards. It was developed in wolges (src/census.rs, and lab/leavegen.txt
// for the method and its measurements); this is a port.
//
// Its catalog is the multiset lattice: every multiset of 0 to rack_size tiles
// over num_letters tile kinds (the blank included), each given a rank by
// formula, with no stored table. The multisets are ordered by size first, then
// within a size by a combinatorial number system over their letter counts. For
// English (27 kinds, 7 tiles) there are 5,379,616 of them.

enum {
  CENSUS_MAX_LETTERS = 64,
};

// The rank of a tally the lattice does not hold.
#define CENSUS_UNRANKED UINT32_MAX

// Any value below every reachable equity, and far enough above INT32_MIN that
// adding a leave value to it cannot overflow.
#define CENSUS_UNPLAYABLE (INT32_MIN / 2)

typedef struct CensusLattice CensusLattice;

CensusLattice *census_lattice_create(int num_letters, int rack_size,
                                     ErrorStack *error_stack);
void census_lattice_destroy(CensusLattice *lattice);
int census_lattice_get_num_letters(const CensusLattice *lattice);
int census_lattice_get_rack_size(const CensusLattice *lattice);
// The number of multisets in the lattice.
uint32_t census_lattice_get_size(const CensusLattice *lattice);
// The multisets of `size` tiles are the ranks from
// census_lattice_get_size_start(size) up to
// census_lattice_get_size_start(size + 1), for size up to rack_size.
uint32_t census_lattice_get_size_start(const CensusLattice *lattice, int size);
// Where the full racks, the last block of the lattice, start.
uint32_t census_lattice_get_full_rack_start(const CensusLattice *lattice);
// The rank of the multiset with tally[ml] copies of each ml, or
// CENSUS_UNRANKED if it holds more than rack_size tiles.
uint32_t census_lattice_rank(const CensusLattice *lattice,
                             const uint8_t *tally);
// The tally of the multiset of rank `index`.
void census_lattice_unrank(const CensusLattice *lattice, uint32_t index,
                           uint8_t *tally);

// add(index, ml) is the rank of multiset(index) with one more ml, for every
// index below the full racks. Every hot loop walks the lattice through this
// table instead of ranking: for English it is 27 letters of 1.1M rows, about
// 120 MB. It is stored a letter at a time, so the sweeps of the lattice that
// add one letter to every multiset read it in order.
typedef struct CensusAddTable {
  uint32_t rows;
  uint32_t *add;
} CensusAddTable;

CensusAddTable *census_add_table_create(const CensusLattice *lattice,
                                        int number_of_threads);
void census_add_table_destroy(CensusAddTable *add_table);

static inline uint32_t census_add_table_add(const CensusAddTable *add_table,
                                            uint32_t index, int ml) {
  return add_table->add[(uint64_t)ml * (uint64_t)add_table->rows + index];
}

// One placed tile of a play, for census_record_blank_variants: its letter and
// how much the play's score drops if that tile is a blank.
typedef struct CensusPlacedTile {
  uint8_t ml;
  Equity blank_drop;
} CensusPlacedTile;

// Records, into play_scores, every way the play could be spelled with blanks:
// play_scores[rank of the placed multiset] = max(itself, score). The play was
// generated with a blank only where no real copy of its letter is left, and
// real_score counts every placed tile at face value. Blanking a placed tile
// only removes its own face value (the blank keeps every multiplier and the
// bingo bonus), so the drops are independent and add up; for each count of
// blanks spent on one letter, the cheapest positions are blanked first.
// unseen_tally[ml] is the pool's count of each letter, and num_blanks the
// blanks the pool can spend on one play (at most rack_size). Sorts `placed`.
void census_record_blank_variants(const CensusLattice *lattice,
                                  Equity *play_scores, Equity real_score,
                                  CensusPlacedTile *placed,
                                  int number_of_placed,
                                  const uint8_t *unseen_tally, int num_blanks);

// The max over subsets: dst[S] = the largest src[T] over every T within S.
void census_max_over_subsets(const CensusLattice *lattice,
                             const CensusAddTable *add_table, const Equity *src,
                             Equity *dst);

// A subrack's sums over the racks it is in: their weighted best values and
// their weights. The two sit together, so the superset sum reads one cache
// line a multiset, not two.
typedef struct CensusSums {
  double num;
  double den;
} CensusSums;

typedef struct CensusApportionArgs {
  const CensusLattice *lattice;
  const CensusAddTable *add_table;
  // play_scores[P]: the best score of a play placing exactly the multiset P,
  // never below 0 (any group can be exchanged away for nothing).
  const Equity *play_scores;
  // leave[K]: the value of keeping K, from the previous generation.
  const Equity *leave;
  // census_max_over_subsets of leave, which is the same on every board
  // of a generation, or NULL to compute it here.
  const Equity *leave_max_over_subsets;
  // The board's unseen pool, as a tally.
  const uint8_t *unseen_tally;
  // Whether every leave value is 0.
  bool null_leave;
  // Seed the full racks and fold them down with a superset sum, instead of
  // pushing every drawable rack into each of its subracks. The same values;
  // faster from a pool of about 36 tiles.
  bool superset_sum;
  // Fill each rack's best from the words each spells, instead of searching
  // each rack's splits. The same values; faster except on the largest
  // alphabets. Needs superset_sum.
  bool scatter;
} CensusApportionArgs;

// For every full rack R the board's pool can deal, best(R) is the largest
// play_scores[P] + leave[K] over R = P + K, and its weight w(R) the number of
// ways to draw R from the pool. Adds w(R) * best(R) to sums[S].num and w(R) to
// sums[S].den for every subrack S of R, the empty set and R itself included.
// sums must start at zero; rack_best is scratch, one entry per multiset.
void census_apportion(const CensusApportionArgs *args, CensusSums *sums,
                      Equity *rack_best);

#endif
