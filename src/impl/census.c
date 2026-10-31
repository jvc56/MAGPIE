#include "census.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/equity.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct CensusLattice {
  int num_letters;
  int rack_size;
  // binomials[n * binomial_stride + k] = C(n, k)
  uint64_t *binomials;
  int binomial_stride;
  // size_offsets[size] is where the multisets of that size start, for size up
  // to rack_size + 1 (where the lattice ends).
  uint64_t size_offsets[CENSUS_MAX_LETTERS + 2];
  // A multiset's rank is its size's start plus a term a letter, set by the
  // letter, its count and how many of the multiset's tiles are at that letter
  // or after: rank_terms[(ml * (rack_size + 1) + unplaced) * (rack_size + 1) +
  // count].
  uint32_t *rank_terms;
};

static inline uint64_t lattice_choose(const CensusLattice *lattice, int n,
                                      int k) {
  return lattice
      ->binomials[(size_t)n * (size_t)lattice->binomial_stride + (size_t)k];
}

static inline uint32_t lattice_rank_term(const CensusLattice *lattice, int ml,
                                         int unplaced, int count) {
  const int stride = lattice->rack_size + 1;
  return lattice->rank_terms[(ml * stride + unplaced) * stride + count];
}

CensusLattice *census_lattice_create(int num_letters, int rack_size,
                                     ErrorStack *error_stack) {
  if (num_letters < 1 || num_letters > CENSUS_MAX_LETTERS) {
    error_stack_push(
        error_stack, ERROR_STATUS_CENSUS_INVALID_LATTICE,
        get_formatted_string("a census counts 1 to %d letters, not %d",
                             CENSUS_MAX_LETTERS, num_letters));
    return NULL;
  }
  if (rack_size < 1 || rack_size > CENSUS_MAX_LETTERS) {
    error_stack_push(
        error_stack, ERROR_STATUS_CENSUS_INVALID_LATTICE,
        get_formatted_string("a census holds racks of 1 to %d tiles, not %d",
                             CENSUS_MAX_LETTERS, rack_size));
    return NULL;
  }
  CensusLattice *lattice = malloc_or_die(sizeof(CensusLattice));
  lattice->num_letters = num_letters;
  lattice->rack_size = rack_size;
  const int max_n = rack_size + num_letters;
  lattice->binomial_stride = max_n + 1;
  lattice->rank_terms = NULL;
  lattice->binomials = calloc_or_die((size_t)(max_n + 1) * (size_t)(max_n + 1),
                                     sizeof(uint64_t));
  for (int n = 0; n <= max_n; n++) {
    uint64_t *row =
        lattice->binomials + (size_t)n * (size_t)lattice->binomial_stride;
    row[0] = 1;
    for (int k = 1; k <= n; k++) {
      const uint64_t *previous_row = row - lattice->binomial_stride;
      row[k] = previous_row[k - 1] + previous_row[k];
    }
  }
  for (int size = 0; size <= rack_size + 1; size++) {
    lattice->size_offsets[size] =
        lattice_choose(lattice, size + num_letters - 1, num_letters);
  }
  if (lattice->size_offsets[rack_size + 1] >= CENSUS_UNRANKED) {
    error_stack_push(
        error_stack, ERROR_STATUS_CENSUS_INVALID_LATTICE,
        get_formatted_string("a census of %d letters and %d tiles holds more "
                             "multisets than it can number",
                             num_letters, rack_size));
    census_lattice_destroy(lattice);
    return NULL;
  }
  // The copies of letter ml, each counting the multisets of the remaining
  // tiles over the letters after it. The last letter's terms are all 0.
  const int stride = rack_size + 1;
  lattice->rank_terms = calloc_or_die(
      (size_t)num_letters * (size_t)stride * (size_t)stride, sizeof(uint32_t));
  for (int ml = 0; ml < num_letters - 1; ml++) {
    const int parts = num_letters - 1 - ml;
    for (int unplaced = 0; unplaced <= rack_size; unplaced++) {
      uint32_t term = 0;
      for (int count = 1; count <= unplaced; count++) {
        term += (uint32_t)lattice_choose(
            lattice, unplaced - (count - 1) + parts - 1, parts - 1);
        lattice->rank_terms[(ml * stride + unplaced) * stride + count] = term;
      }
    }
  }
  return lattice;
}

void census_lattice_destroy(CensusLattice *lattice) {
  if (!lattice) {
    return;
  }
  free(lattice->binomials);
  free(lattice->rank_terms);
  free(lattice);
}

int census_lattice_get_num_letters(const CensusLattice *lattice) {
  return lattice->num_letters;
}

int census_lattice_get_rack_size(const CensusLattice *lattice) {
  return lattice->rack_size;
}

uint32_t census_lattice_get_size(const CensusLattice *lattice) {
  return (uint32_t)lattice->size_offsets[lattice->rack_size + 1];
}

uint32_t census_lattice_get_size_start(const CensusLattice *lattice, int size) {
  return (uint32_t)lattice->size_offsets[size];
}

uint32_t census_lattice_get_full_rack_start(const CensusLattice *lattice) {
  return (uint32_t)lattice->size_offsets[lattice->rack_size];
}

uint32_t census_lattice_rank(const CensusLattice *lattice,
                             const uint8_t *tally) {
  const int num_letters = lattice->num_letters;
  int size = 0;
  for (int ml = 0; ml < num_letters; ml++) {
    size += tally[ml];
  }
  if (size > lattice->rack_size) {
    return CENSUS_UNRANKED;
  }
  uint32_t within = 0;
  int remaining = size;
  for (int ml = 0; ml < num_letters - 1 && remaining > 0; ml++) {
    within += lattice_rank_term(lattice, ml, remaining, tally[ml]);
    remaining -= tally[ml];
  }
  return (uint32_t)lattice->size_offsets[size] + within;
}

void census_lattice_unrank(const CensusLattice *lattice, uint32_t index,
                           uint8_t *tally) {
  const int num_letters = lattice->num_letters;
  int size = 0;
  while (size < lattice->rack_size &&
         lattice->size_offsets[size + 1] <= index) {
    size++;
  }
  uint64_t rest = index - lattice->size_offsets[size];
  int remaining = size;
  for (int ml = 0; ml < num_letters - 1; ml++) {
    const int parts = num_letters - 1 - ml;
    int count = 0;
    while (true) {
      const uint64_t ways =
          lattice_choose(lattice, remaining - count + parts - 1, parts - 1);
      if (rest < ways) {
        break;
      }
      rest -= ways;
      count++;
    }
    tally[ml] = (uint8_t)count;
    remaining -= count;
  }
  tally[num_letters - 1] = (uint8_t)remaining;
}

typedef struct AddTableWorker {
  const CensusLattice *lattice;
  CensusAddTable *add_table;
  uint32_t start_row;
  uint32_t end_row;
} AddTableWorker;

static void *add_table_worker(void *arg) {
  const AddTableWorker *worker = (const AddTableWorker *)arg;
  const int num_letters = worker->lattice->num_letters;
  uint8_t tally[CENSUS_MAX_LETTERS];
  for (uint32_t row = worker->start_row; row < worker->end_row; row++) {
    census_lattice_unrank(worker->lattice, row, tally);
    for (int ml = 0; ml < num_letters; ml++) {
      tally[ml]++;
      worker->add_table->add[(uint64_t)ml * worker->add_table->rows + row] =
          census_lattice_rank(worker->lattice, tally);
      tally[ml]--;
    }
  }
  return NULL;
}

CensusAddTable *census_add_table_create(const CensusLattice *lattice,
                                        int number_of_threads) {
  const int num_letters = lattice->num_letters;
  const uint32_t rows = census_lattice_get_full_rack_start(lattice);
  CensusAddTable *add_table = malloc_or_die(sizeof(CensusAddTable));
  add_table->rows = rows;
  add_table->add =
      malloc_or_die((size_t)rows * (size_t)num_letters * sizeof(uint32_t));
  if (number_of_threads < 1) {
    number_of_threads = 1;
  }
  if ((uint32_t)number_of_threads > rows) {
    number_of_threads = rows > 0 ? (int)rows : 1;
  }
  const uint32_t rows_per_thread =
      (rows + (uint32_t)number_of_threads - 1) / (uint32_t)number_of_threads;
  AddTableWorker *workers =
      malloc_or_die(sizeof(AddTableWorker) * (size_t)number_of_threads);
  cpthread_t *threads =
      malloc_or_die(sizeof(cpthread_t) * (size_t)number_of_threads);
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    uint32_t start_row = (uint32_t)thread_idx * rows_per_thread;
    uint32_t end_row = start_row + rows_per_thread;
    if (start_row > rows) {
      start_row = rows;
    }
    if (end_row > rows) {
      end_row = rows;
    }
    workers[thread_idx] = (AddTableWorker){.lattice = lattice,
                                           .add_table = add_table,
                                           .start_row = start_row,
                                           .end_row = end_row};
    cpthread_create(&threads[thread_idx], add_table_worker,
                    &workers[thread_idx]);
  }
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    cpthread_join(threads[thread_idx]);
  }
  free(threads);
  free(workers);
  return add_table;
}

void census_add_table_destroy(CensusAddTable *add_table) {
  if (!add_table) {
    return;
  }
  free(add_table->add);
  free(add_table);
}

// Blank variants

typedef struct BlankRun {
  uint8_t ml;
  uint8_t start;
  uint8_t count;
  // how many of the run must be blanks, for want of real copies
  uint8_t forced;
} BlankRun;

typedef struct BlankVariantContext {
  const CensusLattice *lattice;
  Equity *play_scores;
  Equity real_score;
  const CensusPlacedTile *placed;
  int number_of_placed;
  const BlankRun *runs;
  int number_of_runs;
} BlankVariantContext;

// Chooses the blanks of runs run_idx and before; the runs after are chosen.
// The blank sorts before every letter, so the tiles at or after a run's letter
// are the real copies of it and of the runs after it, real_after of them, and
// the rank terms (see CensusLattice) of those runs sum to rank_terms.
static void record_blank_variants_recursive(BlankVariantContext *context,
                                            int run_idx, int leftover,
                                            int blanks_total, Equity drop_total,
                                            int real_after,
                                            uint32_t rank_terms) {
  if (run_idx < 0) {
    const int size = blanks_total + real_after;
    const uint32_t rank =
        (uint32_t)context->lattice->size_offsets[size] + rank_terms +
        lattice_rank_term(context->lattice, BLANK_MACHINE_LETTER, size,
                          blanks_total);
    const Equity value = context->real_score - drop_total;
    if (value > context->play_scores[rank]) {
      context->play_scores[rank] = value;
    }
    return;
  }
  const BlankRun *run = &context->runs[run_idx];
  int max_extra = run->count - run->forced;
  if (max_extra > leftover) {
    max_extra = leftover;
  }
  Equity run_drop = 0;
  for (int placed_idx = run->start; placed_idx < run->start + run->forced;
       placed_idx++) {
    run_drop += context->placed[placed_idx].blank_drop;
  }
  for (int extra = 0; extra <= max_extra; extra++) {
    if (extra > 0) {
      run_drop +=
          context->placed[run->start + run->forced + extra - 1].blank_drop;
    }
    const int blanks_in_run = run->forced + extra;
    const int real = run->count - blanks_in_run;
    record_blank_variants_recursive(
        context, run_idx - 1, leftover - extra, blanks_total + blanks_in_run,
        drop_total + run_drop, real_after + real,
        rank_terms + lattice_rank_term(context->lattice, run->ml,
                                       real_after + real, real));
  }
}

static int compare_placed_tiles(const void *first, const void *second) {
  const CensusPlacedTile *first_tile = (const CensusPlacedTile *)first;
  const CensusPlacedTile *second_tile = (const CensusPlacedTile *)second;
  if (first_tile->ml != second_tile->ml) {
    return first_tile->ml < second_tile->ml ? -1 : 1;
  }
  if (first_tile->blank_drop != second_tile->blank_drop) {
    return first_tile->blank_drop < second_tile->blank_drop ? -1 : 1;
  }
  return 0;
}

void census_record_blank_variants(const CensusLattice *lattice,
                                  Equity *play_scores, Equity real_score,
                                  CensusPlacedTile *placed,
                                  int number_of_placed,
                                  const uint8_t *unseen_tally, int num_blanks) {
  const int rack_size = lattice->rack_size;
  if (number_of_placed == 0 || number_of_placed > rack_size) {
    return;
  }
  qsort(placed, (size_t)number_of_placed, sizeof(CensusPlacedTile),
        compare_placed_tiles);
  BlankRun runs[CENSUS_MAX_LETTERS];
  int number_of_runs = 0;
  int total_forced = 0;
  int placed_idx = 0;
  while (placed_idx < number_of_placed) {
    const uint8_t ml = placed[placed_idx].ml;
    const int start = placed_idx;
    while (placed_idx < number_of_placed && placed[placed_idx].ml == ml) {
      placed_idx++;
    }
    const int count = placed_idx - start;
    int real_available = unseen_tally[ml];
    if (real_available > rack_size) {
      real_available = rack_size;
    }
    const int forced = count > real_available ? count - real_available : 0;
    total_forced += forced;
    runs[number_of_runs++] = (BlankRun){.ml = ml,
                                        .start = (uint8_t)start,
                                        .count = (uint8_t)count,
                                        .forced = (uint8_t)forced};
  }
  if (total_forced > num_blanks) {
    return;
  }
  BlankVariantContext context = {.lattice = lattice,
                                 .play_scores = play_scores,
                                 .real_score = real_score,
                                 .placed = placed,
                                 .number_of_placed = number_of_placed,
                                 .runs = runs,
                                 .number_of_runs = number_of_runs};
  record_blank_variants_recursive(&context, number_of_runs - 1,
                                  num_blanks - total_forced, 0, 0, 0, 0);
}

// Apportion

void census_max_over_subsets(const CensusLattice *lattice,
                             const CensusAddTable *add_table, const Equity *src,
                             Equity *dst) {
  const int num_letters = lattice->num_letters;
  const uint32_t full_rack_start = census_lattice_get_full_rack_start(lattice);
  memcpy(dst, src, sizeof(Equity) * census_lattice_get_size(lattice));
  for (int ml = 0; ml < num_letters; ml++) {
    for (uint32_t index = 0; index < full_rack_start; index++) {
      const uint32_t superset = census_add_table_add(add_table, index, ml);
      if (dst[index] > dst[superset]) {
        dst[superset] = dst[index];
      }
    }
  }
}

typedef struct ScatterContext {
  // the lattice's rank_terms and their stride, held here for the hot loop
  const uint32_t *rank_terms;
  int rank_term_stride;
  const Equity *leave;
  Equity *best;
  int num_letters;
  int rack_size;
  uint32_t full_rack_start;
  // of the played tiles P being scattered
  Equity played_value;
  int keep_size;
  uint32_t keep_start;
  const uint8_t *played_tally;
  const uint8_t *played_before;
  // played_tails[kept * (num_letters + 1) + ml]: the rank terms of P's tiles
  // from letter ml on, when `kept` tiles of K come before ml and the rest
  // after
  const uint32_t *played_tails;
  // of the pool after P
  const uint8_t *available;
  const uint32_t *suffix_capacity;
} ScatterContext;

// Raises best[P + K] to play_scores[P] + leave[K], for the whole of K placed
// and its letters and P's before ml summing to keep_terms and rack_terms.
static inline void scatter_leaf(const ScatterContext *context, int ml,
                                uint32_t keep_terms, uint32_t rack_terms) {
  const uint32_t *played_tail =
      context->played_tails +
      (size_t)context->keep_size * (size_t)(context->num_letters + 1);
  const uint32_t rack_index =
      context->full_rack_start + rack_terms + played_tail[ml];
  const Equity candidate =
      context->played_value + context->leave[context->keep_start + keep_terms];
  // Unconditionally: whether a play beats the rack's best so far is a coin
  // flip, and a mispredicted branch discards the leaves' loads in flight.
  Equity *best = &context->best[rack_index];
  const Equity current = *best;
  *best = candidate > current ? candidate : current;
}

// Draws the kept tiles K, `remaining` of them (at least one) from letter ml
// on, beside the played tiles P, and raises best[P + K] to play_scores[P] +
// leave[K]. keep_terms and rack_terms are the rank terms (see CensusLattice)
// of K's and of P + K's letters before ml. Each step places the copies of one
// letter of K, so a letter K skips costs nothing but its share of P's terms,
// and the step that places K's last tiles records the rack in place.
static void scatter_keep(const ScatterContext *context, int ml, int remaining,
                         uint32_t keep_terms, uint32_t rack_terms) {
  const int num_letters = context->num_letters;
  const int kept = context->keep_size - remaining;
  const uint32_t *played_tail =
      context->played_tails + (size_t)kept * (size_t)(num_letters + 1);
  // the tiles of P + K at or after a letter, but for P's before it
  const int unplaced_after_played = context->rack_size - kept;
  for (int next = ml;
       next < num_letters && (int)context->suffix_capacity[next] >= remaining;
       next++) {
    int capacity = context->available[next];
    if (capacity == 0) {
      continue;
    }
    if (capacity > remaining) {
      capacity = remaining;
    }
    const uint32_t skipped = played_tail[ml] - played_tail[next];
    const int unplaced = unplaced_after_played - context->played_before[next];
    const int played = context->played_tally[next];
    // the terms of this letter's copies in K and in P + K, by count
    const int stride = context->rank_term_stride;
    const uint32_t *keep_letter_terms =
        context->rank_terms +
        (size_t)(next * stride + remaining) * (size_t)stride;
    const uint32_t *rack_letter_terms =
        context->rank_terms +
        (size_t)(next * stride + unplaced) * (size_t)stride + played;
    for (int count = 1; count <= capacity; count++) {
      const uint32_t next_keep_terms = keep_terms + keep_letter_terms[count];
      const uint32_t next_rack_terms =
          rack_terms + skipped + rack_letter_terms[count];
      if (count == remaining) {
        scatter_leaf(context, next + 1, next_keep_terms, next_rack_terms);
      } else {
        scatter_keep(context, next + 1, remaining - count, next_keep_terms,
                     next_rack_terms);
      }
    }
  }
}

// For each played group P a word reaches, pushes play_scores[P] + leave[K] onto
// every full rack P + K the pool can deal. best must hold the max over subsets
// of leave, the value of exchanging toward each keep.
static void scatter_words(const CensusLattice *lattice,
                          const Equity *play_scores, const Equity *leave,
                          const uint8_t *unseen_tally, Equity *best) {
  const int num_letters = lattice->num_letters;
  const int rack_size = lattice->rack_size;
  const uint32_t lattice_size = census_lattice_get_size(lattice);
  uint8_t played_tally[CENSUS_MAX_LETTERS] = {0};
  uint8_t played_before[CENSUS_MAX_LETTERS] = {0};
  uint32_t played_tails[(CENSUS_MAX_LETTERS + 1) * (CENSUS_MAX_LETTERS + 1)] = {
      0};
  uint8_t available[CENSUS_MAX_LETTERS] = {0};
  uint32_t suffix_capacity[CENSUS_MAX_LETTERS + 1] = {0};
  ScatterContext context = {.rank_terms = lattice->rank_terms,
                            .rank_term_stride = rack_size + 1,
                            .leave = leave,
                            .best = best,
                            .num_letters = num_letters,
                            .rack_size = rack_size,
                            .full_rack_start =
                                census_lattice_get_full_rack_start(lattice),
                            .played_tally = played_tally,
                            .played_before = played_before,
                            .played_tails = played_tails,
                            .available = available,
                            .suffix_capacity = suffix_capacity};
  for (uint32_t played_index = 1; played_index < lattice_size; played_index++) {
    const Equity played_value = play_scores[played_index];
    if (played_value <= 0) {
      continue;
    }
    census_lattice_unrank(lattice, played_index, played_tally);
    int played_size = 0;
    for (int ml = 0; ml < num_letters; ml++) {
      played_before[ml] = (uint8_t)played_size;
      played_size += played_tally[ml];
    }
    const int keep_size = rack_size - played_size;
    for (int kept = 0; kept <= keep_size; kept++) {
      uint32_t *played_tail =
          played_tails + (size_t)kept * (size_t)(num_letters + 1);
      played_tail[num_letters] = 0;
      for (int ml = num_letters - 1; ml >= 0; ml--) {
        played_tail[ml] =
            played_tail[ml + 1] +
            lattice_rank_term(lattice, ml, rack_size - kept - played_before[ml],
                              played_tally[ml]);
      }
    }
    for (int ml = 0; ml < num_letters; ml++) {
      available[ml] = unseen_tally[ml] > played_tally[ml]
                          ? (uint8_t)(unseen_tally[ml] - played_tally[ml])
                          : 0;
    }
    suffix_capacity[num_letters] = 0;
    for (int ml = num_letters - 1; ml >= 0; ml--) {
      const uint32_t capped =
          available[ml] < keep_size ? available[ml] : (uint32_t)keep_size;
      suffix_capacity[ml] = suffix_capacity[ml + 1] + capped;
    }
    context.played_value = played_value;
    context.keep_size = keep_size;
    context.keep_start = (uint32_t)lattice->size_offsets[keep_size];
    if (keep_size == 0) {
      scatter_leaf(&context, 0, 0, 0);
    } else {
      scatter_keep(&context, 0, keep_size, 0, 0);
    }
  }
}

typedef struct LetterCount {
  int ml;
  int count;
} LetterCount;

typedef struct ApportionContext {
  const CensusApportionArgs *args;
  int num_letters;
  const uint32_t *suffix_capacity;
  CensusSums *sums;
  bool best_from_rack_best;
  const Equity *rack_best;
  // the nonzero letters of the rack being drawn
  LetterCount letters[CENSUS_MAX_LETTERS];
} ApportionContext;

// The best play_scores[P] + leave[K] over the splits of the rack's first
// letter_idx nonzero letters, P and K holding the rest so far.
static void best_split(const ApportionContext *context, int letter_idx,
                       uint32_t played_index, uint32_t keep_index,
                       Equity *best) {
  if (letter_idx == 0) {
    const Equity value = context->args->play_scores[played_index] +
                         context->args->leave[keep_index];
    if (value > *best) {
      *best = value;
    }
    return;
  }
  const CensusAddTable *add_table = context->args->add_table;
  const LetterCount *letter = &context->letters[letter_idx - 1];
  for (int played_count = 0; played_count <= letter->count; played_count++) {
    uint32_t split_keep_index = keep_index;
    for (int kept_idx = 0; kept_idx < letter->count - played_count;
         kept_idx++) {
      split_keep_index =
          census_add_table_add(add_table, split_keep_index, letter->ml);
    }
    best_split(context, letter_idx - 1, played_index, split_keep_index, best);
    if (played_count < letter->count) {
      played_index = census_add_table_add(add_table, played_index, letter->ml);
    }
  }
}

// Adds the rack's weighted value to every subrack of its first letter_idx
// nonzero letters.
static void apportion_to_subracks(ApportionContext *context, int letter_idx,
                                  uint32_t subrack_index, double weight,
                                  double weighted_equity) {
  if (letter_idx == 0) {
    context->sums[subrack_index].num += weighted_equity;
    context->sums[subrack_index].den += weight;
    return;
  }
  const LetterCount *letter = &context->letters[letter_idx - 1];
  uint32_t index = subrack_index;
  for (int subrack_count = 0; subrack_count <= letter->count; subrack_count++) {
    apportion_to_subracks(context, letter_idx - 1, index, weight,
                          weighted_equity);
    if (subrack_count < letter->count) {
      index = census_add_table_add(context->args->add_table, index, letter->ml);
    }
  }
}

// Every full rack the pool can deal, `remaining` tiles still to draw from
// letter ml on: weight is the ways to draw what is drawn so far, index its
// rank, and number_of_letters its nonzero letters so far.
static void enumerate_drawable(ApportionContext *context, int ml, int remaining,
                               double weight, uint32_t index,
                               int number_of_letters) {
  if (remaining == 0) {
    Equity best;
    if (context->best_from_rack_best) {
      best = context->rack_best[index];
    } else {
      best = CENSUS_UNPLAYABLE;
      best_split(context, number_of_letters, 0, 0, &best);
    }
    const double best_double = (double)best;
    if (context->args->superset_sum) {
      // seed the superset sum at the full rack
      context->sums[index].num = weight * best_double;
      context->sums[index].den = weight;
    } else {
      apportion_to_subracks(context, number_of_letters, 0, weight,
                            weight * best_double);
    }
    return;
  }
  if (ml == context->num_letters ||
      (int)context->suffix_capacity[ml] < remaining) {
    return;
  }
  enumerate_drawable(context, ml + 1, remaining, weight, index,
                     number_of_letters);
  const int unseen = context->args->unseen_tally[ml];
  const int capacity = unseen < remaining ? unseen : remaining;
  double binomial = 1.0;
  uint32_t drawn_index = index;
  for (int count = 1; count <= capacity; count++) {
    binomial = binomial * (double)(unseen - count + 1) / (double)count;
    drawn_index =
        census_add_table_add(context->args->add_table, drawn_index, ml);
    context->letters[number_of_letters] =
        (LetterCount){.ml = ml, .count = count};
    enumerate_drawable(context, ml + 1, remaining - count, weight * binomial,
                       drawn_index, number_of_letters + 1);
  }
}

// The superset sum: folds every multiset's sums into each of its subsets, one
// sweep of the lattice a letter.
static void sum_over_supersets(ApportionContext *context) {
  const CensusAddTable *add_table = context->args->add_table;
  const uint32_t full_rack_start =
      census_lattice_get_full_rack_start(context->args->lattice);
  for (int ml = 0; ml < context->num_letters; ml++) {
    for (uint32_t index = full_rack_start; index-- > 0;) {
      const uint32_t superset = census_add_table_add(add_table, index, ml);
      context->sums[index].num += context->sums[superset].num;
      context->sums[index].den += context->sums[superset].den;
    }
  }
}

void census_apportion(const CensusApportionArgs *args, CensusSums *sums,
                      Equity *rack_best) {
  const CensusLattice *lattice = args->lattice;
  const int num_letters = lattice->num_letters;
  const int rack_size = lattice->rack_size;

  const bool max_over_subsets = args->null_leave && args->superset_sum;
  const bool scatter_active =
      args->scatter && !args->null_leave && args->superset_sum;
  if (max_over_subsets) {
    census_max_over_subsets(lattice, args->add_table, args->play_scores,
                            rack_best);
  } else if (scatter_active) {
    if (args->leave_max_over_subsets) {
      memcpy(rack_best, args->leave_max_over_subsets,
             sizeof(Equity) * census_lattice_get_size(lattice));
    } else {
      census_max_over_subsets(lattice, args->add_table, args->leave, rack_best);
    }
    scatter_words(lattice, args->play_scores, args->leave, args->unseen_tally,
                  rack_best);
  }

  uint32_t suffix_capacity[CENSUS_MAX_LETTERS + 1];
  suffix_capacity[num_letters] = 0;
  for (int ml = num_letters - 1; ml >= 0; ml--) {
    const uint32_t capped = args->unseen_tally[ml] < rack_size
                                ? args->unseen_tally[ml]
                                : (uint32_t)rack_size;
    suffix_capacity[ml] = suffix_capacity[ml + 1] + capped;
  }
  ApportionContext context = {.args = args,
                              .num_letters = num_letters,
                              .suffix_capacity = suffix_capacity,
                              .sums = sums,
                              .best_from_rack_best =
                                  max_over_subsets || scatter_active,
                              .rack_best = rack_best};
  enumerate_drawable(&context, 0, rack_size, 1.0, 0, 0);
  if (args->superset_sum) {
    sum_over_supersets(&context);
  }
}
