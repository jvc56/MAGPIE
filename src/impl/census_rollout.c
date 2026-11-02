#include "census_rollout.h"

#include "../compat/memory_info.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/static_eval_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/peg_adjust.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
#include "../util/io_util.h"
#include "census.h"
#include "census_play_scores.h"
#include "gameplay.h"
#include "move_gen.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  CENSUS_ROLLOUT_MAX_UNITS = CENSUS_ROLLOUT_NUM_LANES * BOARD_DIM,
  CENSUS_ROLLOUT_NO_UNIT = -1,
  // Racks whose root-table bests a worker remembers (a power of two).
  CENSUS_ROLLOUT_ROOT_SLOTS = 1 << 13,
  // A rank's run of entries in a table: its first entry in the low bits, its
  // count of entries above them.
  CENSUS_ROLLOUT_RUN_COUNT_SHIFT = 24,
  CENSUS_ROLLOUT_RUN_MAX_COUNT = (1 << (32 - CENSUS_ROLLOUT_RUN_COUNT_SHIFT)),
  // The radix sort of a table's entries by rank: two digits.
  CENSUS_ROLLOUT_RADIX_BITS = 12,
  CENSUS_ROLLOUT_RADIX_SIZE = 1 << CENSUS_ROLLOUT_RADIX_BITS,
};

// An anchor or lane with no play: below every equity a move has.
#define CENSUS_ROLLOUT_NO_PLAY EQUITY_MIN_VALUE

// A lane's anchor columns are bits of a uint32_t, and the lanes bits of a
// uint64_t.
static_assert(BOARD_DIM < 32, "census rollout column masks are 32 bits");
static_assert(CENSUS_ROLLOUT_NUM_LANES <= 64,
              "census rollout lane masks are 64 bits");

// A census board's per-anchor play scores: for every placed multiset P, the
// anchors (units) it is played from and its best score from each, as entries
// ordered by P's lattice rank. The tables are sparse (a few percent of the
// lattice's ranks have entries), so a rank's run is found from a bitmap of
// the ranks with entries rather than a lattice-sized array.
typedef struct CensusRolloutTable {
  int number_of_units;
  int16_t unit_of[CENSUS_ROLLOUT_NUM_LANES][BOARD_DIM];
  uint16_t *units;
  Equity *scores;
  uint32_t number_of_entries;
  uint32_t entries_capacity;
  // The compact table: a bit for every rank, set for those with entries; by
  // 64-bit word, the set bits before it; and for each set bit, in rank order,
  // its rank's first entry and count of entries (see
  // CENSUS_ROLLOUT_RUN_COUNT_SHIFT).
  uint64_t *present;
  uint32_t *word_runs;
  uint32_t *runs;
  uint32_t runs_capacity;
} CensusRolloutTable;

typedef struct CensusRolloutBoard {
  Game *game;
  uint64_t hashes[CENSUS_ROLLOUT_NUM_LANES];
  uint32_t anchor_cols[CENSUS_ROLLOUT_NUM_LANES];
  // For a full rack, the zone of each anchor (see
  // census_rollout_anchor_zones).
  uint32_t zones[CENSUS_ROLLOUT_NUM_LANES][BOARD_DIM];
  // The anchors whose bests are the root table's: every anchor of the root,
  // and those of a candidate's board that the candidate leaves as they were.
  uint32_t from_root[CENSUS_ROLLOUT_NUM_LANES];
  // The other anchors' scores, and the anchors they are built for.
  CensusRolloutTable table;
  uint32_t build_cols[CENSUS_ROLLOUT_NUM_LANES];
  // Its place among the candidates, which index each worker's lane memo.
  int index;
} CensusRolloutBoard;

// What a thread builds a table with.
typedef struct CensusRolloutBuilder {
  // Lattice-sized, CENSUS_UNPLAYABLE between builds.
  Equity *scratch;
  CensusTouched touched;
} CensusRolloutBuilder;

struct CensusRollout {
  // The threads that build a sim's tables, at most
  // CENSUS_ROLLOUT_MAX_BUILD_THREADS.
  int number_of_build_threads;
  CensusLattice *lattice;
  // By lattice rank, the leave value of the multiset.
  Equity *leave;
  const KLV *leave_klv;
  // One per thread that builds tables.
  CensusRolloutBuilder builders[CENSUS_ROLLOUT_MAX_BUILD_THREADS];
  CensusRolloutBoard root;
  // The sim's candidates' boards, and their signatures in the same order, for
  // finding a rollout's board. Both grow with the largest sim so far.
  CensusRolloutBoard *candidates;
  uint64_t *signatures;
  int number_of_candidates;
  int candidates_capacity;
  // The board a candidate is played on, while building.
  Game *candidate_game;
  // The sims prepared so far: a worker forgets what it remembered when this
  // changes.
  uint64_t sim_number;
};

// The last changed version of a candidate's lane seen in a rollout (its hash
// and the rack size its zones were for) and what it gave: the anchors it has,
// and those whose plays are as they were on the candidate's board. A lane
// that hashes the same holds the same squares.
typedef struct CensusRolloutLaneMemo {
  uint64_t hash;
  int rack_tiles;
  uint32_t anchors;
  uint32_t known;
} CensusRolloutLaneMemo;

struct CensusRolloutWorker {
  // The census board of the rollout this worker is playing.
  const CensusRolloutBoard *current;
  // The sim_number of the census whose sim the worker remembers.
  uint64_t sim_number;
  // Each rack's bests in the root table's anchors, which hold for the whole
  // sim whatever the candidate: every candidate's rollouts deal the opponent
  // the same racks. Keyed by two independent hashes of the rack and the bag's
  // adjustment class.
  uint64_t root_keys[CENSUS_ROLLOUT_ROOT_SLOTS];
  uint64_t root_checks[CENSUS_ROLLOUT_ROOT_SLOTS];
  Equity *root_cache;
  int root_cache_units;
  Equity root_bests[CENSUS_ROLLOUT_MAX_UNITS];
  Equity candidate_bests[CENSUS_ROLLOUT_MAX_UNITS];
  // By candidate, then lane, for memo_capacity candidates.
  CensusRolloutLaneMemo (*memos)[CENSUS_ROLLOUT_NUM_LANES];
  int memo_capacity;
  // Whether the last generation was served, and steered (see
  // census_rollout_worker_last_served).
  bool last_served;
  bool last_steered;
};

// A table with no entries, for a gather that skips the root's.
static const CensusRolloutTable census_rollout_empty_table;

// A hash of a move's placement, score and equity, which tells candidates
// apart.
static uint64_t census_rollout_move_signature(const Move *move) {
  uint64_t hash = UINT64_C(1469598103934665603);
  const uint64_t prime = UINT64_C(1099511628211);
  hash = (hash ^ (uint64_t)move_get_type(move)) * prime;
  hash = (hash ^ (uint64_t)move_get_row_start(move)) * prime;
  hash = (hash ^ (uint64_t)move_get_col_start(move)) * prime;
  hash = (hash ^ (uint64_t)move_get_dir(move)) * prime;
  const int length = move_get_tiles_length(move);
  hash = (hash ^ (uint64_t)length) * prime;
  for (int tile_idx = 0; tile_idx < length; tile_idx++) {
    hash = (hash ^ (uint64_t)move_get_tile(move, tile_idx)) * prime;
  }
  hash = (hash ^ (uint64_t)(uint32_t)move_get_score(move)) * prime;
  hash = (hash ^ (uint64_t)(uint32_t)move_get_equity(move)) * prime;
  return hash;
}

// A lane's squares as the generator reads them (lane BOARD_DIM * dir + row).
static const Square *census_rollout_lane_squares(const Game *game, int lane) {
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), 0);
  return board_get_row_cache(
      board_get_readonly_lanes(game_get_board(game), cross_index),
      lane % BOARD_DIM, lane / BOARD_DIM);
}

static bool square_is_empty(const Square *square) {
  return square_get_letter(square) == ALPHABET_EMPTY_SQUARE_MARKER;
}

// The columns of a lane's anchors.
static uint32_t census_rollout_anchor_cols(const Square *squares) {
  uint32_t anchor_cols = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    if (square_get_anchor(&squares[col])) {
      anchor_cols |= (UINT32_C(1) << col);
    }
  }
  return anchor_cols;
}

// The squares of a lane whose letter, cross set, cross score, cross word flag
// or anchor differ between the two boards: what decides which plays a lane
// holds and what they score, square by square.
static uint32_t census_rollout_changed_cols(const Square *now,
                                            const Square *then) {
  uint32_t changed = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    if (square_get_letter(&now[col]) != square_get_letter(&then[col]) ||
        square_get_cross_set(&now[col]) != square_get_cross_set(&then[col]) ||
        square_get_cross_score(&now[col]) !=
            square_get_cross_score(&then[col]) ||
        square_get_is_cross_word(&now[col]) !=
            square_get_is_cross_word(&then[col]) ||
        square_get_anchor(&now[col]) != square_get_anchor(&then[col])) {
      changed |= (UINT32_C(1) << col);
    }
  }
  return changed;
}

// The columns from low to high. Both are columns, so neither shift is
// negative or past BOARD_DIM, which is below 32.
static uint32_t cols_between(int low, int high) {
  // NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift)
  return ((UINT32_C(1) << (high + 1)) - 1) & ~((UINT32_C(1) << low) - 1);
}

// For each anchor of a lane, the squares its plays can cover or border: from
// one left of the farthest start to one right of the farthest end that
// rack_tiles tiles reach on either side, the anchor before it bounding it on
// the left as it bounds the generator. A play's legality and score read only
// the squares it covers and borders, so a changed square outside an anchor's
// zone leaves its plays as they were.
static void census_rollout_anchor_zones(const Square *squares,
                                        uint32_t anchor_cols, int rack_tiles,
                                        uint32_t *zones) {
  int last_anchor_col = BOARD_DIM;
  for (uint32_t remaining = anchor_cols; remaining != 0;
       remaining &= remaining - 1) {
    const int col = __builtin_ctz(remaining);
    const bool on_tile = !square_is_empty(&squares[col]);
    const int left_limit =
        last_anchor_col == BOARD_DIM ? 0 : last_anchor_col + 1;
    int spare = rack_tiles - (on_tile ? 0 : 1);
    if (spare < 0) {
      spare = 0;
    }
    int start = col;
    int tiles = spare;
    for (int other = col - 1; other >= left_limit; other--) {
      if (square_is_empty(&squares[other])) {
        if (tiles == 0) {
          break;
        }
        tiles--;
      }
      start = other;
    }
    int end = col;
    tiles = spare;
    for (int other = col + 1; other < BOARD_DIM; other++) {
      if (square_is_empty(&squares[other])) {
        if (tiles == 0) {
          break;
        }
        tiles--;
      }
      end = other;
    }
    zones[col] = cols_between(start > 0 ? start - 1 : 0,
                              end < BOARD_DIM - 1 ? end + 1 : BOARD_DIM - 1);
    last_anchor_col = on_tile ? col + 1 : col;
  }
}

// Always stores, so the compiler selects instead of branching: whether a
// value raises the best is data-dependent and mispredicts as a branch.
static void raise_best(Equity *best, Equity value) {
  const Equity old = *best;
  *best = value > old ? value : old;
}

static void census_rollout_table_destroy(CensusRolloutTable *table) {
  free(table->present);
  free(table->word_runs);
  free(table->runs);
  free(table->units);
  free(table->scores);
}

static void census_rollout_board_destroy(CensusRolloutBoard *board) {
  game_destroy(board->game);
  census_rollout_table_destroy(&board->table);
}

bool census_rollout_supports(const Game *game) {
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  const int number_of_letters = ld_get_size(game_get_ld(game));
  return game_get_variant(game) == GAME_VARIANT_CLASSIC &&
         game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG) &&
         game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KLV) && klv != NULL &&
         number_of_letters >= 1 && number_of_letters <= CENSUS_MAX_LETTERS;
}

uint64_t census_rollout_memory_bytes(const Game *game, int number_of_threads,
                                     int number_of_candidates) {
  if (!census_rollout_supports(game)) {
    return 0;
  }
  ErrorStack *error_stack = error_stack_create();
  CensusLattice *lattice = census_lattice_create(ld_get_size(game_get_ld(game)),
                                                 RACK_SIZE, error_stack);
  error_stack_destroy(error_stack);
  if (lattice == NULL) {
    return 0;
  }
  const uint64_t lattice_size = census_lattice_get_size(lattice);
  census_lattice_destroy(lattice);
  if (number_of_threads < 1) {
    number_of_threads = 1;
  }
  if (number_of_candidates < 0) {
    number_of_candidates = 0;
  }
  const uint64_t number_of_boards = 1 + (uint64_t)number_of_candidates;
  uint64_t build_threads = (uint64_t)number_of_threads;
  if (build_threads > number_of_boards) {
    build_threads = number_of_boards;
  }
  if (build_threads > CENSUS_ROLLOUT_MAX_BUILD_THREADS) {
    build_threads = CENSUS_ROLLOUT_MAX_BUILD_THREADS;
  }
  uint64_t root_units = 0;
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    root_units += (uint64_t)__builtin_popcount(
        census_rollout_anchor_cols(census_rollout_lane_squares(game, lane)));
  }
  // The leave values, and each build thread's scratch play scores.
  const uint64_t lattice_tables =
      sizeof(Equity) * lattice_size * (1 + build_threads);
  // Every board, the root's and each candidate's: its table and its game.
  // With their spare capacity, the tables take from under 1 MB to about
  // 5.5 MB a board on CSW24, against a lattice of 5.4M ranks: the most on an
  // open board with a full bag, which a byte a rank about matches, and far
  // less later in the game. A byte a rank again allows for each build
  // thread's sort.
  const uint64_t boards = lattice_size * (number_of_boards + build_threads) +
                          number_of_boards * sizeof(Board);
  // Each worker's cache of a rack's bests in the position's anchors, and its
  // memo of every candidate's lanes.
  const uint64_t worker_caches =
      (uint64_t)number_of_threads *
      (sizeof(CensusRolloutWorker) +
       sizeof(Equity) * root_units * CENSUS_ROLLOUT_ROOT_SLOTS +
       sizeof(CensusRolloutLaneMemo) * CENSUS_ROLLOUT_NUM_LANES *
           (uint64_t)number_of_candidates);
  return lattice_tables + boards + worker_caches;
}

bool census_rollout_fits_in_memory(uint64_t census_bytes,
                                   uint64_t physical_bytes,
                                   int64_t available_bytes) {
  if (census_bytes > physical_bytes / 4) {
    return false;
  }
  if (available_bytes < 0) {
    return true;
  }
  return (uint64_t)available_bytes >= CENSUS_ROLLOUT_MEMORY_RESERVE_BYTES &&
         census_bytes <=
             (uint64_t)available_bytes - CENSUS_ROLLOUT_MEMORY_RESERVE_BYTES;
}

bool census_rollout_auto_fits(const Game *game, int number_of_threads,
                              int number_of_candidates, uint64_t held_bytes) {
  const uint64_t census_bytes = census_rollout_memory_bytes(
      game, number_of_threads, number_of_candidates);
  if (census_bytes == 0) {
    return false;
  }
  int64_t available_bytes = -1;
  if (!get_available_memory_bytes(&available_bytes)) {
    available_bytes = -1;
  }
  // What the census holds is free to it.
  if (available_bytes >= 0) {
    available_bytes += (int64_t)held_bytes;
  }
  return census_rollout_fits_in_memory(
      census_bytes, get_search_memory_budget_base(), available_bytes);
}

// The bytes of a table's arrays, number_of_words being its bitmap's.
static uint64_t census_rollout_table_held_bytes(const CensusRolloutTable *table,
                                                uint64_t number_of_words) {
  uint64_t bytes =
      (uint64_t)table->entries_capacity * (sizeof(uint16_t) + sizeof(Equity)) +
      (uint64_t)table->runs_capacity * sizeof(uint32_t);
  if (table->present != NULL) {
    bytes += number_of_words * (sizeof(uint64_t) + sizeof(uint32_t));
  }
  return bytes;
}

static uint64_t census_rollout_board_held_bytes(const CensusRolloutBoard *board,
                                                uint64_t number_of_words) {
  return census_rollout_table_held_bytes(&board->table, number_of_words) +
         (board->game != NULL ? sizeof(Board) : 0);
}

uint64_t census_rollout_held_bytes(const CensusRollout *census) {
  if (census == NULL) {
    return 0;
  }
  uint64_t bytes = sizeof(CensusRollout);
  if (census->lattice == NULL) {
    return bytes;
  }
  const uint64_t lattice_size = census_lattice_get_size(census->lattice);
  const uint64_t number_of_words = lattice_size / 64 + 1;
  bytes += sizeof(Equity) * lattice_size;
  for (int builder_idx = 0; builder_idx < CENSUS_ROLLOUT_MAX_BUILD_THREADS;
       builder_idx++) {
    const CensusRolloutBuilder *builder = &census->builders[builder_idx];
    if (builder->scratch != NULL) {
      bytes += sizeof(Equity) * lattice_size;
    }
    bytes += sizeof(uint32_t) * (uint64_t)builder->touched.capacity;
  }
  bytes += census_rollout_board_held_bytes(&census->root, number_of_words);
  for (int candidate_idx = 0; candidate_idx < census->candidates_capacity;
       candidate_idx++) {
    bytes += sizeof(CensusRolloutBoard) + sizeof(uint64_t) +
             census_rollout_board_held_bytes(&census->candidates[candidate_idx],
                                             number_of_words);
  }
  if (census->candidate_game != NULL) {
    bytes += sizeof(Board);
  }
  return bytes;
}

CensusRollout *census_rollout_create(int number_of_build_threads) {
  CensusRollout *census = calloc_or_die(1, sizeof(CensusRollout));
  census->number_of_build_threads = number_of_build_threads;
  return census;
}

void census_rollout_destroy(CensusRollout *census) {
  if (census == NULL) {
    return;
  }
  census_rollout_board_destroy(&census->root);
  for (int candidate_idx = 0; candidate_idx < census->candidates_capacity;
       candidate_idx++) {
    census_rollout_board_destroy(&census->candidates[candidate_idx]);
  }
  free(census->candidates);
  free(census->signatures);
  game_destroy(census->candidate_game);
  census_lattice_destroy(census->lattice);
  free(census->leave);
  for (int builder_idx = 0; builder_idx < CENSUS_ROLLOUT_MAX_BUILD_THREADS;
       builder_idx++) {
    free(census->builders[builder_idx].scratch);
    free(census->builders[builder_idx].touched.ranks);
  }
  free(census);
}

static void census_rollout_ensure_lattice(CensusRollout *census,
                                          const Game *game) {
  if (census->lattice != NULL) {
    return;
  }
  ErrorStack *error_stack = error_stack_create();
  census->lattice = census_lattice_create(ld_get_size(game_get_ld(game)),
                                          RACK_SIZE, error_stack);
  error_stack_destroy(error_stack);
  if (census->lattice == NULL) {
    log_fatal("census rollout: no census lattice for this alphabet");
    return;
  }
  const uint32_t size = census_lattice_get_size(census->lattice);
  census->leave = malloc_or_die(sizeof(Equity) * size);
}

static void census_rollout_table_append(CensusRolloutTable *table,
                                        uint32_t rank, int unit, Equity score,
                                        uint32_t **ranks) {
  if (table->number_of_entries == table->entries_capacity) {
    table->entries_capacity =
        table->entries_capacity ? table->entries_capacity * 2 : 1 << 16;
    table->units = realloc_or_die(table->units,
                                  sizeof(uint16_t) * table->entries_capacity);
    table->scores =
        realloc_or_die(table->scores, sizeof(Equity) * table->entries_capacity);
    *ranks = realloc_or_die(*ranks, sizeof(uint32_t) * table->entries_capacity);
  }
  (*ranks)[table->number_of_entries] = rank;
  table->units[table->number_of_entries] = (uint16_t)unit;
  table->scores[table->number_of_entries] = score;
  table->number_of_entries++;
}

// The entries of rank in a compact table, from first to last: none when its
// bit is clear, without a branch.
static void census_rollout_table_find(const CensusRolloutTable *table,
                                      uint32_t rank, uint32_t *first,
                                      uint32_t *last) {
  const uint32_t word_idx = rank >> 6;
  const int bit = (int)(rank & 63);
  const uint64_t word = table->present[word_idx];
  const uint32_t run_idx =
      table->word_runs[word_idx] +
      (uint32_t)__builtin_popcountll(word & ((UINT64_C(1) << bit) - 1));
  const uint32_t run = table->runs[run_idx];
  const uint32_t present = (uint32_t)(word >> bit) & 1;
  *first = run & ((UINT32_C(1) << CENSUS_ROLLOUT_RUN_COUNT_SHIFT) - 1);
  *last = *first + present * (run >> CENSUS_ROLLOUT_RUN_COUNT_SHIFT);
}

// One digit of an LSD radix sort of the entries by rank, from one set of
// arrays into the other.
static void census_rollout_radix_pass(uint32_t number_of_entries, int shift,
                                      const uint32_t *from_ranks,
                                      const uint16_t *from_units,
                                      const Equity *from_scores,
                                      uint32_t *to_ranks, uint16_t *to_units,
                                      Equity *to_scores) {
  uint32_t starts[CENSUS_ROLLOUT_RADIX_SIZE] = {0};
  for (uint32_t entry = 0; entry < number_of_entries; entry++) {
    starts[(from_ranks[entry] >> shift) & (CENSUS_ROLLOUT_RADIX_SIZE - 1)]++;
  }
  uint32_t total = 0;
  for (int digit = 0; digit < CENSUS_ROLLOUT_RADIX_SIZE; digit++) {
    const uint32_t count = starts[digit];
    starts[digit] = total;
    total += count;
  }
  for (uint32_t entry = 0; entry < number_of_entries; entry++) {
    const uint32_t position = starts[(from_ranks[entry] >> shift) &
                                     (CENSUS_ROLLOUT_RADIX_SIZE - 1)]++;
    to_ranks[position] = from_ranks[entry];
    to_units[position] = from_units[entry];
    to_scores[position] = from_scores[entry];
  }
}

// Orders the table's entries by rank and indexes each rank's run of them.
static void census_rollout_table_compact(CensusRolloutTable *table,
                                         const CensusLattice *lattice,
                                         uint32_t *ranks) {
  const uint32_t number_of_entries = table->number_of_entries;
  if (number_of_entries >= (UINT32_C(1) << CENSUS_ROLLOUT_RUN_COUNT_SHIFT)) {
    log_fatal("census rollout: too many entries for a compact table");
  }
  uint32_t *sorted_ranks =
      malloc_or_die(sizeof(uint32_t) * (number_of_entries + 1));
  uint16_t *sorted_units =
      malloc_or_die(sizeof(uint16_t) * (number_of_entries + 1));
  Equity *sorted_scores =
      malloc_or_die(sizeof(Equity) * (number_of_entries + 1));
  census_rollout_radix_pass(number_of_entries, 0, ranks, table->units,
                            table->scores, sorted_ranks, sorted_units,
                            sorted_scores);
  census_rollout_radix_pass(number_of_entries, CENSUS_ROLLOUT_RADIX_BITS,
                            sorted_ranks, sorted_units, sorted_scores, ranks,
                            table->units, table->scores);
  free(sorted_scores);
  free(sorted_units);
  free(sorted_ranks);
  const uint32_t size = census_lattice_get_size(lattice);
  const uint32_t number_of_words = size / 64 + 1;
  if (table->present == NULL) {
    table->present = malloc_or_die(sizeof(uint64_t) * number_of_words);
    table->word_runs = malloc_or_die(sizeof(uint32_t) * number_of_words);
  }
  memset(table->present, 0, sizeof(uint64_t) * number_of_words);
  uint32_t number_of_runs = 0;
  uint32_t first = 0;
  while (first < number_of_entries) {
    const uint32_t rank = ranks[first];
    uint32_t last = first + 1;
    while (last < number_of_entries && ranks[last] == rank) {
      last++;
    }
    if (last - first >= CENSUS_ROLLOUT_RUN_MAX_COUNT) {
      log_fatal("census rollout: too many anchors for a rank");
    }
    // One run beyond the last, for a lookup past every set bit.
    if (number_of_runs + 1 >= table->runs_capacity) {
      table->runs_capacity =
          table->runs_capacity ? table->runs_capacity * 2 : 1024;
      table->runs =
          realloc_or_die(table->runs, sizeof(uint32_t) * table->runs_capacity);
    }
    table->present[rank >> 6] |= UINT64_C(1) << (rank & 63);
    table->runs[number_of_runs++] =
        first | ((last - first) << CENSUS_ROLLOUT_RUN_COUNT_SHIFT);
    first = last;
  }
  if (table->runs_capacity == 0) {
    table->runs_capacity = 1024;
    table->runs = malloc_or_die(sizeof(uint32_t) * table->runs_capacity);
  }
  table->runs[number_of_runs] = 0;
  uint32_t runs_before = 0;
  for (uint32_t word_idx = 0; word_idx < number_of_words; word_idx++) {
    table->word_runs[word_idx] = runs_before;
    runs_before += (uint32_t)__builtin_popcountll(table->present[word_idx]);
  }
}

// Builds the play scores of the anchors in build_cols, by lane, of the board,
// over its pool, sparsely: each anchor's build resets only what it wrote.
static void census_rollout_table_build(const CensusRollout *census,
                                       CensusRolloutBuilder *builder,
                                       CensusRolloutTable *table,
                                       const Game *game,
                                       const uint32_t *build_cols) {
  if (builder->scratch == NULL) {
    const uint32_t size = census_lattice_get_size(census->lattice);
    builder->scratch = malloc_or_die(sizeof(Equity) * size);
    for (uint32_t index = 0; index < size; index++) {
      builder->scratch[index] = CENSUS_UNPLAYABLE;
    }
  }
  uint8_t pool[CENSUS_MAX_LETTERS];
  census_get_unseen_tally(game, pool);
  table->number_of_units = 0;
  table->number_of_entries = 0;
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      table->unit_of[lane][col] = CENSUS_ROLLOUT_NO_UNIT;
    }
  }
  uint32_t *ranks = NULL;
  const uint32_t saved_capacity = table->entries_capacity;
  // census_rollout_table_append grows ranks with the table; start it at the
  // table's size.
  if (saved_capacity > 0) {
    ranks = malloc_or_die(sizeof(uint32_t) * saved_capacity);
  }
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    for (uint32_t remaining = build_cols[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      const int unit = table->number_of_units++;
      table->unit_of[lane][col] = (int16_t)unit;
      builder->touched.count = 0;
      census_play_scores_build_anchor(game, census->lattice, pool,
                                      builder->scratch, &builder->touched, lane,
                                      col);
      for (uint32_t touched_idx = 0; touched_idx < builder->touched.count;
           touched_idx++) {
        const uint32_t rank = builder->touched.ranks[touched_idx];
        const Equity score = builder->scratch[rank];
        if (score > 0) {
          census_rollout_table_append(table, rank, unit, score, &ranks);
        }
        builder->scratch[rank] = CENSUS_UNPLAYABLE;
      }
    }
  }
  census_rollout_table_compact(table, census->lattice, ranks);
  free(ranks);
}

static void census_rollout_board_load(CensusRolloutBoard *board,
                                      const Game *game) {
  if (board->game == NULL) {
    board->game = game_duplicate(game);
  } else {
    game_copy(board->game, game);
  }
  gen_hash_game_lanes(game, 0, board->hashes);
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    const Square *squares = census_rollout_lane_squares(game, lane);
    board->anchor_cols[lane] = census_rollout_anchor_cols(squares);
    census_rollout_anchor_zones(squares, board->anchor_cols[lane], RACK_SIZE,
                                board->zones[lane]);
  }
}

// The lattice, and the leave values of the first player's leaves.
static void census_rollout_ensure_leaves(CensusRollout *census,
                                         const Game *game) {
  census_rollout_ensure_lattice(census, game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  if (klv != census->leave_klv) {
    census_leaves_from_klv(census->lattice, klv, census->leave);
    census->leave_klv = klv;
  }
}

static void census_rollout_set_root(CensusRollout *census, const Game *game) {
  CensusRolloutBoard *root = &census->root;
  census_rollout_board_load(root, game);
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    root->from_root[lane] = root->anchor_cols[lane];
    root->build_cols[lane] = root->anchor_cols[lane];
  }
  census->number_of_candidates = 0;
}

// Loads the board after candidate, played on game, and the anchors whose play
// scores its table holds: those the candidate changes.
static void census_rollout_add_candidate(CensusRollout *census,
                                         const Game *game,
                                         const Move *candidate) {
  if (census->candidate_game == NULL) {
    census->candidate_game = game_duplicate(game);
  } else {
    game_copy(census->candidate_game, game);
  }
  play_move(candidate, census->candidate_game, NULL);
  const Game *board_game = census->candidate_game;
  const CensusRolloutBoard *root = &census->root;
  CensusRolloutBoard *board = &census->candidates[census->number_of_candidates];
  board->index = census->number_of_candidates;
  census->number_of_candidates++;
  census_rollout_board_load(board, board_game);
  census->signatures[board->index] = census_rollout_move_signature(candidate);
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    board->from_root[lane] = 0;
    if (board->hashes[lane] == root->hashes[lane]) {
      board->from_root[lane] = board->anchor_cols[lane];
    } else {
      const uint32_t changed = census_rollout_changed_cols(
          census_rollout_lane_squares(board_game, lane),
          census_rollout_lane_squares(root->game, lane));
      for (uint32_t remaining =
               board->anchor_cols[lane] & root->anchor_cols[lane];
           remaining != 0; remaining &= remaining - 1) {
        const int col = __builtin_ctz(remaining);
        if ((board->zones[lane][col] & changed) == 0 &&
            (root->zones[lane][col] & changed) == 0) {
          board->from_root[lane] |= (UINT32_C(1) << col);
        }
      }
    }
    board->build_cols[lane] =
        board->anchor_cols[lane] & ~board->from_root[lane];
  }
}

// A thread that builds tables: the root's (index 0) and the candidates', each
// claimed whole from next_board.
typedef struct CensusRolloutBuildWork {
  CensusRollout *census;
  CensusRolloutBuilder *builder;
  atomic_int *next_board;
} CensusRolloutBuildWork;

static void census_rollout_build_boards(const CensusRolloutBuildWork *work) {
  CensusRollout *census = work->census;
  const int number_of_boards = 1 + census->number_of_candidates;
  for (int board_idx = atomic_fetch_add(work->next_board, 1);
       board_idx < number_of_boards;
       board_idx = atomic_fetch_add(work->next_board, 1)) {
    CensusRolloutBoard *board =
        board_idx == 0 ? &census->root : &census->candidates[board_idx - 1];
    census_rollout_table_build(census, work->builder, &board->table,
                               board->game, board->build_cols);
  }
}

static void *census_rollout_build_thread(void *arg) {
  census_rollout_build_boards((const CensusRolloutBuildWork *)arg);
  return NULL;
}

// Builds the tables of the boards from first_board (0 the root, then the
// candidates), on up to number_of_build_threads threads.
static void census_rollout_build_tables(CensusRollout *census,
                                        int first_board) {
  const int number_of_boards = 1 + census->number_of_candidates - first_board;
  int number_of_threads = census->number_of_build_threads;
  if (number_of_threads > number_of_boards) {
    number_of_threads = number_of_boards;
  }
  if (number_of_threads > CENSUS_ROLLOUT_MAX_BUILD_THREADS) {
    number_of_threads = CENSUS_ROLLOUT_MAX_BUILD_THREADS;
  }
  if (number_of_threads < 1) {
    number_of_threads = 1;
  }
  atomic_int next_board;
  atomic_init(&next_board, first_board);
  CensusRolloutBuildWork work[CENSUS_ROLLOUT_MAX_BUILD_THREADS];
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    work[thread_idx] =
        (CensusRolloutBuildWork){.census = census,
                                 .builder = &census->builders[thread_idx],
                                 .next_board = &next_board};
  }
  census_run_phase(work, sizeof(CensusRolloutBuildWork), number_of_threads,
                   census_rollout_build_thread);
}

// Makes room for count candidates' boards, keeping those already loaded.
static void census_rollout_reserve_candidates(CensusRollout *census,
                                              int count) {
  if (count <= census->candidates_capacity) {
    return;
  }
  census->candidates = realloc_or_die(
      census->candidates, sizeof(CensusRolloutBoard) * (size_t)count);
  memset(&census->candidates[census->candidates_capacity], 0,
         sizeof(CensusRolloutBoard) *
             (size_t)(count - census->candidates_capacity));
  census->signatures =
      realloc_or_die(census->signatures, sizeof(uint64_t) * (size_t)count);
  census->candidates_capacity = count;
}

// Loads the root's board and the candidates', and the anchors each builds.
static void census_rollout_load_boards(CensusRollout *census, const Game *game,
                                       const MoveList *candidates) {
  census_rollout_set_root(census, game);
  const int count = move_list_get_count(candidates);
  census_rollout_reserve_candidates(census, count);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    census_rollout_add_candidate(census, game,
                                 move_list_get_move(candidates, move_idx));
  }
}

void census_rollout_prepare(CensusRollout *census, const Game *game,
                            const MoveList *candidates) {
  census_rollout_ensure_leaves(census, game);
  census_rollout_load_boards(census, game, candidates);
  census_rollout_build_tables(census, 0);
  census->sim_number++;
}

static int census_rollout_build_anchors(const CensusRolloutBoard *board) {
  int anchors = 0;
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    anchors += __builtin_popcount(board->build_cols[lane]);
  }
  return anchors;
}

// The table entries whose build rollouts rollouts of plies plies pay for.
static uint64_t census_rollout_paid_entries(uint64_t rollouts, int plies) {
  if (plies > CENSUS_ROLLOUT_AUTO_PAYING_PLIES) {
    plies = CENSUS_ROLLOUT_AUTO_PAYING_PLIES;
  }
  if (plies < 1) {
    return 0;
  }
  return CENSUS_ROLLOUT_AUTO_ENTRIES_PER_PLY * rollouts * (uint64_t)plies;
}

bool census_rollout_build_pays(uint64_t root_entries, int root_anchors,
                               int candidate_anchors, uint64_t rollouts,
                               int plies) {
  if (root_anchors <= 0 || candidate_anchors <= 0) {
    return true;
  }
  const uint64_t predicted_entries =
      root_entries * (uint64_t)candidate_anchors / (uint64_t)root_anchors;
  return predicted_entries <= census_rollout_paid_entries(rollouts, plies);
}

bool census_rollout_build_may_pay(uint64_t full_racks, int candidate_anchors,
                                  uint64_t rollouts, int plies) {
  if (candidate_anchors <= 0) {
    return true;
  }
  const uint64_t fewest_entries = (full_racks * (uint64_t)candidate_anchors) >>
                                  CENSUS_ROLLOUT_AUTO_FEWEST_ENTRIES_SHIFT;
  return fewest_entries <= census_rollout_paid_entries(rollouts, plies);
}

// The distinct full racks that can be drawn from the letters the player on
// turn has not seen.
static uint64_t census_rollout_full_racks(const Game *game) {
  uint8_t pool[CENSUS_MAX_LETTERS];
  census_get_unseen_tally(game, pool);
  // By size, the multisets of the letters so far.
  uint64_t multisets[RACK_SIZE + 1] = {1};
  const int number_of_letters = ld_get_size(game_get_ld(game));
  for (int ml = 0; ml < number_of_letters; ml++) {
    for (int size = RACK_SIZE; size > 0; size--) {
      for (int count = 1; count <= pool[ml] && count <= size; count++) {
        multisets[size] += multisets[size - count];
      }
    }
  }
  return multisets[RACK_SIZE];
}

bool census_rollout_prepare_if_it_pays(CensusRollout *census, const Game *game,
                                       const MoveList *candidates,
                                       uint64_t rollouts, int plies) {
  census_rollout_load_boards(census, game, candidates);
  int candidate_anchors = 0;
  for (int candidate_idx = 0; candidate_idx < census->number_of_candidates;
       candidate_idx++) {
    candidate_anchors +=
        census_rollout_build_anchors(&census->candidates[candidate_idx]);
  }
  // Declines without building anything when even the fewest entries the
  // candidates' tables are likely to hold cost too much.
  if (!census_rollout_build_may_pay(census_rollout_full_racks(game),
                                    candidate_anchors, rollouts, plies)) {
    return false;
  }
  census_rollout_ensure_leaves(census, game);
  census_rollout_table_build(census, &census->builders[0], &census->root.table,
                             census->root.game, census->root.build_cols);
  if (!census_rollout_build_pays(census->root.table.number_of_entries,
                                 census_rollout_build_anchors(&census->root),
                                 candidate_anchors, rollouts, plies)) {
    return false;
  }
  census_rollout_build_tables(census, 1);
  census->sim_number++;
  return true;
}

CensusRolloutWorker *census_rollout_worker_create(void) {
  return calloc_or_die(1, sizeof(CensusRolloutWorker));
}

void census_rollout_worker_destroy(CensusRolloutWorker *worker) {
  if (worker == NULL) {
    return;
  }
  free(worker->root_cache);
  free(worker->memos);
  free(worker);
}

uint64_t census_rollout_worker_held_bytes(const CensusRolloutWorker *worker) {
  if (worker == NULL) {
    return 0;
  }
  return sizeof(CensusRolloutWorker) +
         sizeof(*worker->memos) * (uint64_t)worker->memo_capacity +
         sizeof(Equity) * (uint64_t)worker->root_cache_units *
             CENSUS_ROLLOUT_ROOT_SLOTS;
}

// Forgets the last sim's racks and lanes, which this sim's boards and pool
// may not share.
static void census_rollout_worker_start_sim(CensusRolloutWorker *worker,
                                            const CensusRollout *census) {
  memset(worker->root_keys, 0, sizeof(worker->root_keys));
  if (worker->memo_capacity < census->number_of_candidates) {
    free(worker->memos);
    worker->memo_capacity = census->number_of_candidates;
    worker->memos =
        malloc_or_die(sizeof(*worker->memos) * (size_t)worker->memo_capacity);
  }
  // A hash of 0 matches no lane (see census_rollout_skips).
  memset(worker->memos, 0,
         sizeof(*worker->memos) * (size_t)census->number_of_candidates);
  const int root_units = census->root.table.number_of_units;
  if (worker->root_cache_units < root_units) {
    free(worker->root_cache);
    worker->root_cache_units = root_units;
    worker->root_cache = malloc_or_die(sizeof(Equity) * (size_t)root_units *
                                       CENSUS_ROLLOUT_ROOT_SLOTS);
  }
  worker->sim_number = census->sim_number;
}

void census_rollout_worker_set_candidate(CensusRolloutWorker *worker,
                                         const CensusRollout *census,
                                         const Move *candidate) {
  if (worker->sim_number != census->sim_number) {
    census_rollout_worker_start_sim(worker, census);
  }
  const uint64_t signature = census_rollout_move_signature(candidate);
  worker->current = NULL;
  for (int candidate_idx = 0; candidate_idx < census->number_of_candidates;
       candidate_idx++) {
    if (census->signatures[candidate_idx] == signature) {
      worker->current = &census->candidates[candidate_idx];
      return;
    }
  }
}

// The rack's best in each unit of the two tables: root_bests by the root's
// units, candidate_bests by the candidate's, from every split of the rack
// (CensusRackSplitter).
static void census_rollout_gather(const CensusRollout *census, const Rack *rack,
                                  const CensusRolloutTable *root_table,
                                  const CensusRolloutTable *candidate_table,
                                  const Equity *peg_by_tiles_played,
                                  Equity *root_bests, Equity *candidate_bests) {
  for (int unit = 0; unit < root_table->number_of_units; unit++) {
    root_bests[unit] = CENSUS_ROLLOUT_NO_PLAY;
  }
  for (int unit = 0; unit < candidate_table->number_of_units; unit++) {
    candidate_bests[unit] = CENSUS_ROLLOUT_NO_PLAY;
  }
  // An empty table (the root's on a remembered rack) has no entries to read,
  // so skip its lookups.
  const bool use_root = root_table->number_of_entries > 0;
  const bool use_candidate = candidate_table->number_of_entries > 0;
  if (!use_root && !use_candidate) {
    return;
  }
  MachineLetter letters[RACK_SIZE] = {0};
  uint8_t counts[RACK_SIZE] = {0};
  int number_of_letters = 0;
  const int dist_size = rack_get_dist_size(rack);
  for (int ml = 0; ml < dist_size; ml++) {
    const int count = rack_get_letter(rack, ml);
    if (count > 0) {
      letters[number_of_letters] = (MachineLetter)ml;
      counts[number_of_letters] = (uint8_t)count;
      number_of_letters++;
    }
  }
  // Every split's played and kept ranks first, from small tables only, so
  // that the lookups below are independent of each other and overlap
  // instead of each waiting behind the walk's mispredicts.
  uint32_t split_played[CENSUS_MAX_RACK_SPLITS];
  uint32_t split_kept[CENSUS_MAX_RACK_SPLITS];
  uint8_t split_tiles[CENSUS_MAX_RACK_SPLITS];
  CensusRackSplitter splitter;
  census_rack_splitter_init(&splitter, census->lattice, letters, counts,
                            number_of_letters);
  const int number_of_splits =
      census_rack_splitter_next(&splitter, CENSUS_MAX_RACK_SPLITS, split_played,
                                split_kept, split_tiles, NULL);
  uint32_t root_firsts[CENSUS_MAX_RACK_SPLITS];
  uint32_t root_lasts[CENSUS_MAX_RACK_SPLITS];
  uint32_t candidate_firsts[CENSUS_MAX_RACK_SPLITS];
  uint32_t candidate_lasts[CENSUS_MAX_RACK_SPLITS];
  Equity leave_values[CENSUS_MAX_RACK_SPLITS];
  for (int split_idx = 0; split_idx < number_of_splits; split_idx++) {
    const uint32_t played_index = split_played[split_idx];
    root_firsts[split_idx] = 0;
    root_lasts[split_idx] = 0;
    candidate_firsts[split_idx] = 0;
    candidate_lasts[split_idx] = 0;
    if (use_root) {
      census_rollout_table_find(root_table, played_index,
                                &root_firsts[split_idx],
                                &root_lasts[split_idx]);
    }
    if (use_candidate) {
      census_rollout_table_find(candidate_table, played_index,
                                &candidate_firsts[split_idx],
                                &candidate_lasts[split_idx]);
    }
    leave_values[split_idx] = census->leave[split_kept[split_idx]] +
                              peg_by_tiles_played[split_tiles[split_idx]];
  }
  for (int split_idx = 0; split_idx < number_of_splits; split_idx++) {
    const Equity leave_value = leave_values[split_idx];
    for (uint32_t entry = root_firsts[split_idx]; entry < root_lasts[split_idx];
         entry++) {
      raise_best(&root_bests[root_table->units[entry]],
                 root_table->scores[entry] + leave_value);
    }
    for (uint32_t entry = candidate_firsts[split_idx];
         entry < candidate_lasts[split_idx]; entry++) {
      raise_best(&candidate_bests[candidate_table->units[entry]],
                 candidate_table->scores[entry] + leave_value);
    }
  }
}

void census_rollout_root_cache_key(int bag_class, const Rack *rack,
                                   uint64_t *key, uint64_t *check) {
  // The class goes through a multiply before the rack's counts do. Combined
  // with the first count, the blank's, before any multiply, it reached the
  // words only through class ^ blanks and class + blanks, so a class and a
  // blank count could trade off: class 2 with a blank and class 3 without, or
  // class 0 with two blanks and class 2 without, the other tiles the same,
  // shared both words.
  uint64_t key_word = (UINT64_C(0xCBF29CE484222325) ^ (uint64_t)bag_class) *
                      UINT64_C(0x100000001B3);
  uint64_t check_word = (UINT64_C(0x84222325CBF29CE4) + (uint64_t)bag_class) *
                        UINT64_C(0x9E3779B97F4A7C15);
  for (int ml = 0; ml < rack_get_dist_size(rack); ml++) {
    key_word = (key_word ^ rack_get_letter(rack, ml)) * UINT64_C(0x100000001B3);
    check_word = (check_word + rack_get_letter(rack, ml) + 1) *
                 UINT64_C(0x9E3779B97F4A7C15);
  }
  *key = key_word | 1;
  *check = check_word;
}

// The rack's bests in every anchor of both tables, the root's from the
// worker's memory when the rack has been gathered this sim.
static void census_rollout_gather_rack(CensusRolloutWorker *worker,
                                       const CensusRollout *census,
                                       const Game *game, const Rack *rack,
                                       const CensusRolloutBoard *board) {
  const int bag = bag_get_letters(game_get_bag(game));
  // By tiles played, the pre-endgame adjustment of a play from this bag, as
  // move generation adds it. The census serves only players without a table
  // (see census_rollout_serves), so these are all 0.
  Equity peg_by_tiles_played[RACK_SIZE + 1];
  peg_adjust_fill_by_tiles_played(
      player_get_peg_adjust(
          game_get_player(game, game_get_player_on_turn_index(game))),
      bag, peg_by_tiles_played);
  // Bags that give no play an adjustment share the root bests.
  const int bag_class = bag >= PEG_ADJUST_VALUES_LENGTH ? 0 : bag;
  uint64_t key;
  uint64_t check;
  census_rollout_root_cache_key(bag_class, rack, &key, &check);
  const int slot = (int)(key & (CENSUS_ROLLOUT_ROOT_SLOTS - 1));
  Equity *cached =
      &worker->root_cache[(size_t)slot * (size_t)worker->root_cache_units];
  const int root_units = census->root.table.number_of_units;
  if (worker->root_keys[slot] == key && worker->root_checks[slot] == check) {
    census_rollout_gather(census, rack, &census_rollout_empty_table,
                          &board->table, peg_by_tiles_played,
                          worker->root_bests, worker->candidate_bests);
    memcpy(worker->root_bests, cached, sizeof(Equity) * (size_t)root_units);
  } else {
    census_rollout_gather(census, rack, &census->root.table, &board->table,
                          peg_by_tiles_played, worker->root_bests,
                          worker->candidate_bests);
    memcpy(cached, worker->root_bests, sizeof(Equity) * (size_t)root_units);
    worker->root_keys[slot] = key;
    worker->root_checks[slot] = check;
  }
}

// The skips the census gives a generation of game's player on turn on the
// current board, whose census board is the worker's current one: every anchor
// known to be below the best known anchor. Returns false if it knows no
// anchor.
static bool census_rollout_skips(CensusRolloutWorker *worker,
                                 const CensusRollout *census, const Game *game,
                                 const uint64_t *hashes, uint64_t *skip_lanes,
                                 uint32_t *skip_anchor_cols) {
  const CensusRolloutBoard *board = worker->current;
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  const int rack_tiles = rack_get_total_letters(rack);
  census_rollout_gather_rack(worker, census, game, rack, board);
  uint32_t known[CENSUS_ROLLOUT_NUM_LANES];
  Equity values[CENSUS_ROLLOUT_NUM_LANES][BOARD_DIM];
  uint32_t anchors_now[CENSUS_ROLLOUT_NUM_LANES];
  Equity floor_equity = CENSUS_ROLLOUT_NO_PLAY;
  bool any_known = false;
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    known[lane] = 0;
    anchors_now[lane] = 0;
    if (hashes[lane] == 0) {
      continue;
    }
    if (hashes[lane] == board->hashes[lane]) {
      anchors_now[lane] = board->anchor_cols[lane];
      known[lane] = anchors_now[lane];
    } else if (worker->memos[board->index][lane].hash == hashes[lane] &&
               worker->memos[board->index][lane].rack_tiles == rack_tiles) {
      anchors_now[lane] = worker->memos[board->index][lane].anchors;
      known[lane] = worker->memos[board->index][lane].known;
    } else {
      const Square *squares = census_rollout_lane_squares(game, lane);
      anchors_now[lane] = census_rollout_anchor_cols(squares);
      const uint32_t changed = census_rollout_changed_cols(
          squares, census_rollout_lane_squares(board->game, lane));
      uint32_t zones_now[BOARD_DIM];
      census_rollout_anchor_zones(squares, anchors_now[lane], rack_tiles,
                                  zones_now);
      for (uint32_t remaining = anchors_now[lane] & board->anchor_cols[lane];
           remaining != 0; remaining &= remaining - 1) {
        const int col = __builtin_ctz(remaining);
        if ((zones_now[col] & changed) == 0 &&
            (board->zones[lane][col] & changed) == 0) {
          known[lane] |= (UINT32_C(1) << col);
        }
      }
      worker->memos[board->index][lane] =
          (CensusRolloutLaneMemo){.hash = hashes[lane],
                                  .rack_tiles = rack_tiles,
                                  .anchors = anchors_now[lane],
                                  .known = known[lane]};
    }
    for (uint32_t remaining = known[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      Equity value;
      if (board->from_root[lane] & (UINT32_C(1) << col)) {
        value = worker->root_bests[census->root.table.unit_of[lane][col]];
      } else {
        value = worker->candidate_bests[board->table.unit_of[lane][col]];
      }
      values[lane][col] = value;
      any_known = true;
      raise_best(&floor_equity, value);
    }
  }
  if (!any_known) {
    return false;
  }
  *skip_lanes = 0;
  for (int lane = 0; lane < CENSUS_ROLLOUT_NUM_LANES; lane++) {
    uint32_t skip = 0;
    for (uint32_t remaining = known[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      if (values[lane][col] == CENSUS_ROLLOUT_NO_PLAY ||
          values[lane][col] < floor_equity) {
        skip |= (UINT32_C(1) << col);
      }
    }
    skip_anchor_cols[lane] = 0;
    if (skip != 0 && skip == anchors_now[lane]) {
      *skip_lanes |= (uint64_t)1 << lane;
    } else {
      skip_anchor_cols[lane] = skip;
    }
  }
  return true;
}

// Whether the census serves a generation from game: a play's equity must be
// its score, its leave's value, and the pre-endgame adjustment, which for a
// given bag depends only on how many tiles the play places (Quackle's
// leftInBagPlusSeven index). With PAT on, it has another term. A player with
// a pre-endgame adjustment table is not served either: the table is per
// player and the root cache's key (bag and rack) does not tell the players
// apart.
static bool census_rollout_serves(const Game *game) {
  if (bag_get_letters(game_get_bag(game)) == 0 ||
      board_get_tiles_played(game_get_board(game)) == 0) {
    return false;
  }
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  return (player_get_pat(player) == NULL || player_get_pat_disabled(player)) &&
         player_get_peg_adjust(player) == NULL;
}

const Move *census_rollout_get_best_move(CensusRolloutWorker *worker,
                                         const CensusRollout *census,
                                         Game *game, MoveList *move_list,
                                         int ply) {
  MoveGenArgs args = {.game = game,
                      .move_list = move_list,
                      .move_record_type = MOVE_RECORD_BEST,
                      .move_sort_type = MOVE_SORT_EQUITY,
                      .override_kwg = NULL,
                      .eq_margin_movegen = 0,
                      .target_equity = EQUITY_MAX_VALUE};
  uint32_t skip_anchor_cols[CENSUS_ROLLOUT_NUM_LANES];
  uint64_t hashes[CENSUS_ROLLOUT_NUM_LANES];
  worker->last_served = worker->current != NULL &&
                        ply <= CENSUS_ROLLOUT_MAX_PLY &&
                        census_rollout_serves(game);
  worker->last_steered = false;
  if (worker->last_served) {
    gen_hash_game_lanes(game, 0, hashes);
    uint64_t skip_lanes = 0;
    if (census_rollout_skips(worker, census, game, hashes, &skip_lanes,
                             skip_anchor_cols)) {
      args.skip_lanes = skip_lanes;
      args.skip_anchor_cols = skip_anchor_cols;
      worker->last_steered = true;
    }
  }
  generate_moves(&args);
  return move_list_get_move(move_list, 0);
}

bool census_rollout_worker_last_served(const CensusRolloutWorker *worker) {
  return worker->last_served;
}

bool census_rollout_worker_last_steered(const CensusRolloutWorker *worker) {
  return worker->last_steered;
}
