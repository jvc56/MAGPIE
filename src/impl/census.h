#ifndef CENSUS_H
#define CENSUS_H

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/equity.h"
#include "../ent/klv.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The leave census was originally developed by Andy Kurnia in wolges; this is
// a port. For the method and its measurements see:
// https://github.com/andy-k/wolges/blob/main/lab/leavegen.txt
// and for the implementation:
// https://github.com/andy-k/wolges/blob/main/src/census.rs
//
// The census values every leave exactly on one board, then averages over
// many boards.
//
// Its catalog is the multiset lattice: every multiset of 0 to rack_size tiles
// over num_letters tile kinds (the blank included), each given a rank by
// formula, with no stored table. The multisets are ordered by size first, then
// within a size by a combinatorial number system over their letter counts. For
// English (27 kinds, 7 tiles) there are 5,379,616 of them.

enum {
  CENSUS_MAX_LETTERS = 64,
  // A rack's splits into played and kept tiles, at most (census_rack_splits).
  CENSUS_MAX_RACK_SPLITS = 1 << RACK_SIZE,
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
// census_lattice_rank of the multiset with counts[idx] copies of
// letters[idx], for number_of_letters letters in increasing order, every
// other letter absent: the same rank, in time set by number_of_letters
// rather than the lattice's num_letters.
uint32_t census_lattice_rank_sparse(const CensusLattice *lattice,
                                    const MachineLetter *letters,
                                    const uint8_t *counts,
                                    int number_of_letters);
// A rack's splits into the tiles a play uses and the tiles it keeps: every
// way to keep from none to all of the copies of each letter, except keeping
// every tile. The rack's distinct letters are letters, in increasing order,
// with counts copies of each, at most RACK_SIZE tiles in all. The splits come
// as an odometer over the copies kept counts them, the first letter changing
// fastest, starting from keeping none. The indexes come from small tables
// only, so a caller can then read lattice-sized tables at many splits'
// indexes with the loads overlapping; a caller that may stop early takes
// them a few at a time.
typedef struct CensusRackSplitter {
  const MachineLetter *letters;
  const uint8_t *counts;
  int number_of_letters;
  // The lattice's rank terms (census_lattice_rank_term) and size starts.
  const uint32_t *rank_terms;
  int stride;
  uint32_t size_starts[RACK_SIZE + 1];
  // The copies kept of each letter, and over it and the letters after it,
  // the tiles kept and played and the sums of their rank terms; the letter
  // the next split changes, or -1 once every split has been given.
  uint8_t kept[RACK_SIZE + 1];
  int kept_after[RACK_SIZE + 1];
  int played_after[RACK_SIZE + 1];
  uint32_t kept_terms[RACK_SIZE + 1];
  uint32_t played_terms[RACK_SIZE + 1];
  int changed_idx;
} CensusRackSplitter;

void census_rack_splitter_init(CensusRackSplitter *splitter,
                               const CensusLattice *lattice,
                               const MachineLetter *letters,
                               const uint8_t *counts, int number_of_letters);

// The next splits, at most max_splits of them: for each, the lattice index of
// the tiles played and of the tiles kept, the number of tiles played, and, if
// kept_counts is not NULL, the copies kept of each letter. Returns how many
// it wrote, 0 once every split has been given. Inline, so that a caller's
// walk keeps its sums in registers.
static inline int census_rack_splitter_next(CensusRackSplitter *splitter,
                                            int max_splits,
                                            uint32_t *played_indexes,
                                            uint32_t *kept_indexes,
                                            uint8_t *tiles_played,
                                            uint8_t (*kept_counts)[RACK_SIZE]) {
  const MachineLetter *letters = splitter->letters;
  const uint8_t *counts = splitter->counts;
  const int number_of_letters = splitter->number_of_letters;
  const uint32_t *rank_terms = splitter->rank_terms;
  const int stride = splitter->stride;
  uint8_t *kept = splitter->kept;
  int number_of_splits = 0;
  // A multiset's rank adds a term for each letter, set by the tiles at that
  // letter or after it, so each split recomputes only the letters at or
  // below the one that changed.
  while (splitter->changed_idx >= 0 && number_of_splits < max_splits) {
    const int changed_idx = splitter->changed_idx;
    // The sums carried in locals, so that each letter does not wait on the
    // store of the one after it.
    int kept_sum = splitter->kept_after[changed_idx + 1];
    int played_sum = splitter->played_after[changed_idx + 1];
    uint32_t kept_terms_sum = splitter->kept_terms[changed_idx + 1];
    uint32_t played_terms_sum = splitter->played_terms[changed_idx + 1];
    for (int letter_idx = changed_idx; letter_idx >= 0; letter_idx--) {
      const int kept_count = kept[letter_idx];
      const int played_count = counts[letter_idx] - kept_count;
      const int letter_base = letters[letter_idx] * stride;
      kept_sum += kept_count;
      played_sum += played_count;
      kept_terms_sum +=
          rank_terms[(letter_base + kept_sum) * stride + kept_count];
      played_terms_sum +=
          rank_terms[(letter_base + played_sum) * stride + played_count];
      splitter->kept_after[letter_idx] = kept_sum;
      splitter->played_after[letter_idx] = played_sum;
      splitter->kept_terms[letter_idx] = kept_terms_sum;
      splitter->played_terms[letter_idx] = played_terms_sum;
    }
    // Keeping every tile is no split.
    if (played_sum > 0) {
      played_indexes[number_of_splits] =
          splitter->size_starts[played_sum] + played_terms_sum;
      kept_indexes[number_of_splits] =
          splitter->size_starts[kept_sum] + kept_terms_sum;
      tiles_played[number_of_splits] = (uint8_t)played_sum;
      if (kept_counts != NULL) {
        for (int letter_idx = 0; letter_idx < number_of_letters; letter_idx++) {
          kept_counts[number_of_splits][letter_idx] = kept[letter_idx];
        }
      }
      number_of_splits++;
    }
    int letter_idx = 0;
    while (letter_idx < number_of_letters &&
           kept[letter_idx] == counts[letter_idx]) {
      kept[letter_idx] = 0;
      letter_idx++;
    }
    if (letter_idx == number_of_letters) {
      splitter->changed_idx = -1;
    } else {
      kept[letter_idx]++;
      splitter->changed_idx = letter_idx;
    }
  }
  return number_of_splits;
}

// The term letter ml adds to the rank of a multiset holding count copies of
// it and unplaced tiles at it or after it: a multiset's rank is
// census_lattice_get_size_start of its size plus the terms of its letters,
// which lets a walk that adds letters in decreasing order rank as it goes.
uint32_t census_lattice_rank_term(const CensusLattice *lattice,
                                  MachineLetter ml, int unplaced, int count);
// The tally of the multiset of rank `index`.
void census_lattice_unrank(const CensusLattice *lattice, uint32_t index,
                           uint8_t *tally);

// Sets leave[index] to the KLV's value of every multiset of the lattice (0 for
// one it does not hold), in one walk of its word graph.
void census_leaves_from_klv(const CensusLattice *lattice, const KLV *klv,
                            Equity *leave);

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
// census_record_blank_variants into play scores that other threads raise
// with it too, each raised by an atomic max (catomic_max_int32).
void census_record_blank_variants_shared(const CensusLattice *lattice,
                                         Equity *play_scores, Equity real_score,
                                         CensusPlacedTile *placed,
                                         int number_of_placed,
                                         const uint8_t *unseen_tally,
                                         int num_blanks);

// Runs phase on each of number_of_workers workers, worker_size bytes apart
// from workers, each on a thread of its own, the first in the calling
// thread, and returns once every one has.
void census_run_phase(void *workers, size_t worker_size, int number_of_workers,
                      void *(*phase)(void *));

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

// The expected best of a full rack drawn from the pool: total->num is the sum
// over the full racks R the pool can deal of w(R) * best(R), and total->den
// the sum of w(R), with best and w as census_apportion has them. Reads
// play_scores, leave, leave_max_over_subsets (which must be set), unseen_tally
// and add_table from args; rack_best is scratch, one entry per multiset. The
// total is the same on any number of threads.
void census_expected_best(const CensusApportionArgs *args, Equity *rack_best,
                          CensusSums *total, int number_of_threads);

#endif
