#include "rack_info_table_maker.h"

#include "../compat/cpthread.h"
#include "../def/bit_rack_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/klv_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/equity.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/leave_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../ent/rack_info_table.h"
#include "../ent/wmp.h"
#include "../util/io_util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Minimum number of hash buckets
enum { RIT_MIN_BUCKETS = 16 };

static inline uint32_t next_power_of_2(uint32_t n) {
  if (n == 0) {
    return 1;
  }
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

static inline int rit_maker_popcount(unsigned int x) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcount)
  return __builtin_popcount(x);
#else
  int count = 0;
  while (x) {
    x &= x - 1;
    count++;
  }
  return count;
#endif
}

// ============================================================================
// Rack enumeration
// ============================================================================

static void count_racks_recursive(const LetterDistribution *ld, int ld_size,
                                  int ml, int remaining, uint32_t *count) {
  if (remaining == 0) {
    (*count)++;
    return;
  }
  if (ml >= ld_size) {
    return;
  }
  int max_this = ld_get_dist(ld, ml);
  if (max_this > remaining) {
    max_this = remaining;
  }
  for (int num = 0; num <= max_this; num++) {
    count_racks_recursive(ld, ld_size, ml + 1, remaining - num, count);
  }
}

static uint32_t count_all_racks(const LetterDistribution *ld) {
  uint32_t count = 0;
  count_racks_recursive(ld, ld_get_size(ld), 0, RACK_SIZE, &count);
  return count;
}

static void enumerate_racks_recursive(const LetterDistribution *ld, int ld_size,
                                      int ml, int remaining, BitRack *current,
                                      BitRack *out_racks, uint32_t *index) {
  if (remaining == 0) {
    out_racks[*index] = *current;
    (*index)++;
    return;
  }
  if (ml >= ld_size) {
    return;
  }
  int max_this = ld_get_dist(ld, ml);
  if (max_this > remaining) {
    max_this = remaining;
  }
  for (int num = 0; num <= max_this; num++) {
    enumerate_racks_recursive(ld, ld_size, ml + 1, remaining - num, current,
                              out_racks, index);
    if (num < max_this) {
      bit_rack_add_letter(current, ml);
    }
  }
  for (int num = 0; num < max_this; num++) {
    bit_rack_take_letter(current, ml);
  }
}

// ============================================================================
// Per-rack entry computation
//
// For a given rack, recursively enumerate every canonical subrack using the
// same walk as generate_exchange_moves in move_gen.c, and at every terminal
// fill in (a) the leave value for the leave multiset (indexed by the
// LeaveMap canonical index) and, when the table has playthrough coverage
// for this played size, (b) OR a bitmask of letters L such that (played
// subrack + {L}) anagrams to a valid word of length (played_size + 1)
// into entry->playthrough_union[leave_size]. The union collapses the
// per-canonical-subrack data that shadow can actually consume: shadow
// tracks tiles_played as a count, not a specific subrack, so a
// leave_size-indexed OR is the right granularity.
// ============================================================================

typedef struct {
  const KLV *klv;
  const WMP *wmp;
  const LetterDistribution *ld;
  int ld_size;
  uint8_t playthrough_min_played_size;
  // Mutable state, updated as we recurse into a single rack.
  Rack *player_rack;
  Rack *leave;
  LeaveMap *leave_map;
  RackInfoTableEntry *entry;
  // 32-bit leave values computed during recursion, packed to 24-bit at end.
  Equity leaves_temp[RACK_INFO_TABLE_LEAVES_PER_ENTRY];
  bool leave_out_of_range;
  // Track the overall best exchange across all leave sizes.
  Equity best_exchange_equity;
  int best_exchange_tiles_exchanged;
  MachineLetter best_exchange_strip[RACK_SIZE];
} EntryComputeState;

static void compute_entry_recursive(EntryComputeState *state,
                                    uint32_t node_index, uint32_t word_index,
                                    MachineLetter ml) {
  const int ld_size = state->ld_size;
  while (ml < ld_size && rack_get_letter(state->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int played_size = rack_get_total_letters(state->player_rack);
    if (played_size > 0) {
      Equity value = 0;
      if (word_index != KLV_UNFOUND_INDEX) {
        value = klv_get_indexed_leave_value(state->klv, word_index - 1);
      }
      state->leaves_temp[state->leave_map->current_index] = value;
      if (value < RACK_INFO_TABLE_LEAVE_MIN ||
          value > RACK_INFO_TABLE_LEAVE_MAX) {
        state->leave_out_of_range = true;
      }

      // Update best_leaves per leave_size.
      const int leave_size_for_best =
          rit_maker_popcount((unsigned int)state->leave_map->current_index);
      if (value > state->entry->best_leaves[leave_size_for_best]) {
        state->entry->best_leaves[leave_size_for_best] = value;
      }

      // Track the overall best exchange across all leave sizes.
      // Tiebreaker matches compare_moves for exchanges: highest equity
      // (= leave value since score is 0), then fewer tiles exchanged,
      // then lexicographically earlier exchange strip.
      const int tiles_exchanged = RACK_SIZE - leave_size_for_best;
      if (tiles_exchanged > 0) {
        bool update_best_exchange = false;
        if (value > state->best_exchange_equity) {
          update_best_exchange = true;
        } else if (value == state->best_exchange_equity) {
          if (tiles_exchanged < state->best_exchange_tiles_exchanged) {
            update_best_exchange = true;
          } else if (tiles_exchanged == state->best_exchange_tiles_exchanged) {
            // Build the exchange strip and compare lexicographically.
            MachineLetter current_strip[RACK_SIZE];
            int strip_idx = 0;
            for (int strip_ml = 0; strip_ml < state->ld_size; strip_ml++) {
              const int count = rack_get_letter(state->player_rack, strip_ml);
              for (int copy_idx = 0; copy_idx < count; copy_idx++) {
                current_strip[strip_idx++] = (MachineLetter)strip_ml;
              }
            }
            for (int cmp_idx = 0; cmp_idx < tiles_exchanged; cmp_idx++) {
              if (current_strip[cmp_idx] <
                  state->best_exchange_strip[cmp_idx]) {
                update_best_exchange = true;
                break;
              }
              if (current_strip[cmp_idx] >
                  state->best_exchange_strip[cmp_idx]) {
                break;
              }
            }
          }
        }
        if (update_best_exchange) {
          state->best_exchange_equity = value;
          state->best_exchange_tiles_exchanged = tiles_exchanged;
          int strip_idx = 0;
          for (int strip_ml = 0; strip_ml < state->ld_size; strip_ml++) {
            const int count = rack_get_letter(state->player_rack, strip_ml);
            for (int copy_idx = 0; copy_idx < count; copy_idx++) {
              state->best_exchange_strip[strip_idx++] = (MachineLetter)strip_ml;
            }
          }
        }
      }

      if (state->wmp != NULL) {
        // Build the BitRack for the played subrack (state->player_rack
        // holds the "would be played" side at this leaf; state->leave
        // holds the complement "would be the leave after playing it").
        const BitRack played_bit_rack =
            bit_rack_create_from_rack(state->ld, state->player_rack);
        const int leave_size =
            rit_maker_popcount((unsigned int)state->leave_map->current_index);

        // Nonplaythrough existence cache: does this specific size-
        // played_size subrack form a valid played_size-letter word on its
        // own? If so, mark the corresponding bit in
        // nonplaythrough_has_word_of_length_bitmask and track the best
        // leave value over all subracks that satisfy this property.
        // Replaces the per-movegen wmp_move_gen_check_nonplaythrough_
        // existence warmup that otherwise runs C(RACK_SIZE, k) WMP
        // lookups per size k on every movegen call.
        if (played_size >= MINIMUM_WORD_LENGTH && played_size <= BOARD_DIM) {
          const WMPEntry *np_wmp_entry =
              wmp_get_word_entry(state->wmp, &played_bit_rack, played_size);
          if (np_wmp_entry != NULL) {
            state->entry->nonplaythrough_has_word_of_length_bitmask |=
                (uint8_t)(1U << played_size);
            if (value >
                state->entry->nonplaythrough_best_leave_values[leave_size]) {
              state->entry->nonplaythrough_best_leave_values[leave_size] =
                  value;
            }
          }
        }

        if (played_size >= (int)state->playthrough_min_played_size) {
          const int word_length = played_size + 1;
          if (word_length >= MINIMUM_WORD_LENGTH && word_length <= BOARD_DIM) {
            // For each concrete letter L in the alphabet, ask the WMP
            // whether (rack + L) forms a valid word of the target length.
            // wmp_get_word_entry dispatches on the blank count in the
            // query bitrack, so this handles 0-, 1-, and 2-blank racks
            // uniformly.
            uint32_t bitmask = 0;
            const int cap = state->ld_size < BIT_RACK_MAX_ALPHABET_SIZE
                                ? state->ld_size
                                : BIT_RACK_MAX_ALPHABET_SIZE;
            for (int ml_candidate = 1; ml_candidate < cap; ml_candidate++) {
              BitRack query = played_bit_rack;
              bit_rack_add_letter(&query, (MachineLetter)ml_candidate);
              const WMPEntry *wmp_entry =
                  wmp_get_word_entry(state->wmp, &query, word_length);
              if (wmp_entry != NULL) {
                bitmask |= (1U << ml_candidate);
              }
            }
            // OR into the union slot for this leave_size. Multiple
            // canonical subracks with the same
            // popcount(leave_map->current_index) are unioned into the
            // same slot.
            state->entry->playthrough_union[leave_size] |= bitmask;
          }
        }
      }
    }
    return;
  }

  compute_entry_recursive(state, node_index, word_index, ml + 1);

  const uint16_t num_this = rack_get_letter(state->player_rack, ml);
  for (uint16_t tile_idx = 0; tile_idx < num_this; tile_idx++) {
    rack_add_letter(state->leave, ml);
    leave_map_take_letter_and_update_complement_index(state->leave_map,
                                                      state->player_rack, ml);
    uint32_t sibling_word_index;
    node_index = increment_node_to_ml(state->klv, node_index, word_index,
                                      &sibling_word_index, ml);
    word_index = sibling_word_index;
    uint32_t child_word_index;
    node_index =
        follow_arc(state->klv, node_index, word_index, &child_word_index);
    word_index = child_word_index;
    compute_entry_recursive(state, node_index, word_index, ml + 1);
  }

  rack_take_letters(state->leave, ml, num_this);
  for (int tile_idx = 0; tile_idx < num_this; tile_idx++) {
    leave_map_add_letter_and_update_complement_index(state->leave_map,
                                                     state->player_rack, ml);
  }
}

static void compute_entry_for_rack(const KLV *klv, const WMP *wmp,
                                   const LetterDistribution *ld,
                                   uint8_t playthrough_min_played_size,
                                   const BitRack *bit_rack,
                                   RackInfoTableEntry *entry) {
  const int ld_size = ld_get_size(ld);

  // Reconstruct Rack from BitRack.
  Rack player_rack;
  rack_set_dist_size(&player_rack, ld_size);
  rack_reset(&player_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = bit_rack_get_letter(bit_rack, ml);
    player_rack.array[ml] = (uint16_t)count;
    player_rack.number_of_letters += (uint16_t)count;
  }

  LeaveMap leave_map;
  leave_map.rack_array_size = ld_size;
  leave_map_init(&player_rack, &leave_map);
  leave_map_set_current_index(&leave_map, 0);

  Rack leave;
  rack_set_dist_size(&leave, ld_size);
  rack_reset(&leave);

  memset(entry->leaves_packed, 0, RACK_INFO_TABLE_LEAVES_PACKED_BYTES);
  memset(entry->playthrough_union, 0,
         RACK_INFO_TABLE_UNIONS_PER_ENTRY * sizeof(uint32_t));
  memset(entry->multi_pt_tp7_bitvec, 0,
         RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS * sizeof(uint32_t));
  memset(entry->multi_pt_tp6_bitvec, 0,
         RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS * sizeof(uint32_t));
  entry->nonplaythrough_has_word_of_length_bitmask = 0;
  memset(entry->pad, 0, sizeof(entry->pad));
  entry->num_bingo_words = 0;
  memset(entry->bingo_words, 0, sizeof(entry->bingo_words));
  memset(entry->best_exchange_strip, 0, sizeof(entry->best_exchange_strip));
  entry->best_exchange_tiles_exchanged = 0;
  for (int leave_idx = 0;
       leave_idx < RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY;
       leave_idx++) {
    entry->nonplaythrough_best_leave_values[leave_idx] = EQUITY_INITIAL_VALUE;
  }
  for (int bl_idx = 0; bl_idx < RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY;
       bl_idx++) {
    entry->best_leaves[bl_idx] = EQUITY_INITIAL_VALUE;
  }

  EntryComputeState state = {
      .klv = klv,
      .wmp = wmp,
      .ld = ld,
      .ld_size = ld_size,
      .playthrough_min_played_size = playthrough_min_played_size,
      .player_rack = &player_rack,
      .leave = &leave,
      .leave_map = &leave_map,
      .entry = entry,
      .best_exchange_equity = EQUITY_INITIAL_VALUE,
      .best_exchange_tiles_exchanged = 0,
  };

  const uint32_t root = kwg_get_dawg_root_node_index(klv_get_kwg(klv));
  compute_entry_recursive(&state, root, 0, 0);

  // Pack 32-bit leave values to 24-bit in the entry.
  if (state.leave_out_of_range) {
    log_fatal("leave value out of 24-bit range during RIT construction");
  }
  for (int leave_idx = 0; leave_idx < RACK_INFO_TABLE_LEAVES_PER_ENTRY;
       leave_idx++) {
    rack_info_table_entry_pack_leave(entry, leave_idx,
                                     state.leaves_temp[leave_idx]);
  }

  // Populate inline bingo words (nonplaythrough full-rack anagrams).
  if (wmp != NULL && (entry->nonplaythrough_has_word_of_length_bitmask &
                      (1U << RACK_SIZE)) != 0) {
    BitRack full_bit_rack = bit_rack_create_from_rack(ld, &player_rack);
    MachineLetter buf[WMP_RESULT_BUFFER_SIZE];
    const int bytes =
        wmp_write_words_to_buffer(wmp, &full_bit_rack, RACK_SIZE, buf);
    const int num_words = bytes / RACK_SIZE;
    if (num_words > 0 && num_words <= RIT_MAX_INLINE_BINGO_WORDS) {
      entry->num_bingo_words = (uint8_t)num_words;
      memcpy(entry->bingo_words, buf, num_words * RACK_SIZE);
    }
  }

  // Copy the overall best exchange into the entry.
  if (state.best_exchange_tiles_exchanged > 0) {
    memcpy(entry->best_exchange_strip, state.best_exchange_strip,
           state.best_exchange_tiles_exchanged * sizeof(MachineLetter));
    entry->best_exchange_tiles_exchanged =
        (uint8_t)state.best_exchange_tiles_exchanged;
  }
}

// ============================================================================
// Multi-playthrough bitvec flipped walk (tp=6, tp=7)
//
// For each tp value N in {6, 7} we want, per 7-tile rack R, a length-
// indexed bitvec where bit L in slot k is set iff there exists a word M
// of length (N + 1 + k) such that some N-tile subrack S of R satisfies
// S ⊆ M and L ∈ (M - S).
//
// Phase 1 (build per-N temp tables, deduped by canonical N-tile S):
//   Enumerate every canonical N-tile multiset S allowed by the LD into a
//   temporary hash table TempSubrackTable[N], one entry per S with a
//   `result[NUM_LENGTHS]` slot. Then walk the WMP's blankless
//   word_map_entries: for each word M of length W and each canonical
//   N-tile submultiset S of M's multiset, OR (M - S)'s letter mask into
//   TempSubrackTable[N][S].result[W - (N + 1)].
//
//   This deduplicates the per-(word, S) work: many words share the same
//   canonical N-tile submultiset, so we only process each unique S once
//   for all (W, letter) bits at the end. (Suggested by jvc56 in PR
//   review thread.)
//
// Phase 2 (distribute to main RIT entries via supersets of each unique S):
//   For each entry (S, result) in TempSubrackTable[N], enumerate every
//   7-tile rack R such that S ⊆ R (R = S + (RACK_SIZE - N) more tiles
//   drawn from the alphabet, respecting the LD's letter caps). For each
//   such R, look up the main RIT entry and OR result[k] into
//   entry->multi_pt_tpN_bitvec[k] for every k.
//
//   The superset enumeration is per-unique-S, so the cost is bounded by
//   (# canonical N-tile multisets) × (# (RACK_SIZE - N)-tile alphabet
//   additions) × (1 RIT lookup + NUM_LENGTHS ORs). Compared to running
//   it per (word, S) pair (which is what a naive single-pass flipped
//   walk would do), this is roughly the same as `# unique S / # (word,
//   S) pairs` cheaper -- a factor of 100-1000x in practice.
//
// Note that for N == RACK_SIZE the superset enumeration is degenerate
// (zero tiles to add, S is already a 7-tile rack), so the same machinery
// handles tp=7 with no special-casing.
// ============================================================================

typedef struct TempSubrackEntry {
  BitRack subrack;
  // Per-length bitvec slots, sized for the largest tp range (tp=6). For
  // tp=7 the unused tail slots stay at 0.
  uint32_t result[RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS];
} TempSubrackEntry;

typedef struct TempSubrackTable {
  TempSubrackEntry *entries;
  uint32_t num_entries;
  uint32_t num_buckets;
  uint32_t *bucket_starts;
} TempSubrackTable;

static uint32_t temp_subrack_table_next_pow2(uint32_t n) {
  if (n == 0) {
    return 1;
  }
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

// Build a hash table of every canonical N-tile multiset allowed by the
// LD. Each entry has a zero-initialised `result[NUM_LENGTHS]` slot ready
// for the per-word OR pass.
static TempSubrackTable *temp_subrack_table_create(const LetterDistribution *ld,
                                                   int subrack_size) {
  TempSubrackTable *table =
      (TempSubrackTable *)calloc_or_die(1, sizeof(TempSubrackTable));
  uint32_t total = 0;
  count_racks_recursive(ld, ld_get_size(ld), 0, subrack_size, &total);
  table->num_entries = total;
  table->entries = (TempSubrackEntry *)calloc_or_die((size_t)total,
                                                     sizeof(TempSubrackEntry));

  BitRack *all_subracks = malloc_or_die((size_t)total * sizeof(BitRack));
  BitRack current = bit_rack_create_empty();
  uint32_t fill_index = 0;
  enumerate_racks_recursive(ld, ld_get_size(ld), 0, subrack_size, &current,
                            all_subracks, &fill_index);

  uint32_t num_buckets = temp_subrack_table_next_pow2(total);
  if (num_buckets < 16) {
    num_buckets = 16;
  }
  table->num_buckets = num_buckets;
  table->bucket_starts =
      (uint32_t *)calloc_or_die((size_t)num_buckets + 1, sizeof(uint32_t));

  uint32_t *bucket_counts =
      (uint32_t *)calloc_or_die((size_t)num_buckets, sizeof(uint32_t));
  for (uint32_t i = 0; i < total; i++) {
    const uint32_t bucket =
        bit_rack_get_bucket_index(&all_subracks[i], num_buckets);
    bucket_counts[bucket]++;
  }
  uint32_t offset = 0;
  for (uint32_t b = 0; b < num_buckets; b++) {
    table->bucket_starts[b] = offset;
    offset += bucket_counts[b];
  }
  table->bucket_starts[num_buckets] = offset;

  uint32_t *bucket_fill =
      (uint32_t *)calloc_or_die((size_t)num_buckets, sizeof(uint32_t));
  for (uint32_t i = 0; i < total; i++) {
    const uint32_t bucket =
        bit_rack_get_bucket_index(&all_subracks[i], num_buckets);
    const uint32_t entry_idx =
        table->bucket_starts[bucket] + bucket_fill[bucket]++;
    table->entries[entry_idx].subrack = all_subracks[i];
    // result[] already zeroed by calloc.
  }
  free(bucket_fill);
  free(bucket_counts);
  free(all_subracks);
  return table;
}

static void temp_subrack_table_destroy(TempSubrackTable *table) {
  if (table == NULL) {
    return;
  }
  free(table->bucket_starts);
  free(table->entries);
  free(table);
}

// Look up the entry for `subrack` in `table`, or NULL if not present.
static inline TempSubrackEntry *
temp_subrack_table_lookup(const TempSubrackTable *table,
                          const BitRack *subrack) {
  const uint32_t bucket =
      bit_rack_get_bucket_index(subrack, table->num_buckets);
  const uint32_t start = table->bucket_starts[bucket];
  const uint32_t end = table->bucket_starts[bucket + 1];
  for (uint32_t i = start; i < end; i++) {
    if (bit_rack_equals(&table->entries[i].subrack, subrack)) {
      return &table->entries[i];
    }
  }
  return NULL;
}

// Configuration for one tp level. Encapsulates everything that varies
// between tp=6 and tp=7 so the OR + distribute machinery below can be
// written once.
typedef struct {
  int subrack_size; // 5, 6, or 7
  int min_word_length;
  int num_word_lengths;
  int max_word_length;
} MultiPtTpConfig;

static const MultiPtTpConfig MULTI_PT_TP_CONFIGS[2] = {
    {RACK_SIZE - 1, RIT_MULTI_PT_TP6_MIN_WORD_LENGTH,
     RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS, RIT_MULTI_PT_TP6_MAX_WORD_LENGTH},
    {RACK_SIZE, RIT_MULTI_PT_TP7_MIN_WORD_LENGTH,
     RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS, RIT_MULTI_PT_TP7_MAX_WORD_LENGTH},
};
enum { NUM_MULTI_PT_TP_CONFIGS = 2 };

typedef struct {
  const uint8_t *word_letter_counts; // BIT_RACK_MAX_ALPHABET_SIZE
  uint8_t submultiset_counts[BIT_RACK_MAX_ALPHABET_SIZE];
  int len_idx;
  TempSubrackTable *temp_table;
} SubmultisetWalkState;

// Phase 1 leaf action: build the k-tile BitRack S from submultiset_counts,
// look up the temp table entry for S, and OR (M - S)'s letter mask into
// the entry's slot for this word length. Repeated lookups for the same S
// across different (M, len_idx) pairs OR into the same uint32 slot.
static void submultiset_walk_leaf(const SubmultisetWalkState *state) {
  BitRack subrack = bit_rack_create_empty();
  for (int ml = 0; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    for (int count_idx = 0; count_idx < state->submultiset_counts[ml];
         count_idx++) {
      bit_rack_add_letter(&subrack, (MachineLetter)ml);
    }
  }
  TempSubrackEntry *entry =
      temp_subrack_table_lookup(state->temp_table, &subrack);
  if (entry == NULL) {
    return;
  }
  uint32_t letter_mask = 0;
  for (int ml = 1; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    if (state->word_letter_counts[ml] > state->submultiset_counts[ml]) {
      letter_mask |= (1U << ml);
    }
  }
  entry->result[state->len_idx] |= letter_mask;
}

static void submultiset_walk_recursive(SubmultisetWalkState *state,
                                       int letter_idx, int tiles_remaining) {
  if (tiles_remaining == 0) {
    submultiset_walk_leaf(state);
    return;
  }
  if (letter_idx >= BIT_RACK_MAX_ALPHABET_SIZE) {
    return;
  }
  const int max_here = state->word_letter_counts[letter_idx] < tiles_remaining
                           ? state->word_letter_counts[letter_idx]
                           : tiles_remaining;
  for (int take = 0; take <= max_here; take++) {
    state->submultiset_counts[letter_idx] = (uint8_t)take;
    submultiset_walk_recursive(state, letter_idx + 1, tiles_remaining - take);
  }
  state->submultiset_counts[letter_idx] = 0;
}

// Phase 1: walk every word in the WMP and accumulate per-canonical-S
// results into the temporary tables for tp=6 and tp=7.
static void phase1_accumulate_temp_tables(
    const WMP *wmp, TempSubrackTable *temp_tables[NUM_MULTI_PT_TP_CONFIGS]) {
  const int min_word_length = RIT_MULTI_PT_TP6_MIN_WORD_LENGTH;
  const int max_word_length = RIT_MULTI_PT_TP7_MAX_WORD_LENGTH;
  for (int word_length = min_word_length; word_length <= max_word_length;
       word_length++) {
    if (word_length > wmp->board_dim) {
      break;
    }
    const WMPForLength *wfl = &wmp->wfls[word_length];
    const uint32_t num_entries = wfl->num_word_entries;
    for (uint32_t wmp_idx = 0; wmp_idx < num_entries; wmp_idx++) {
      const WMPEntry *wmp_entry = &wfl->word_map_entries[wmp_idx];
      const BitRack word_bit_rack = wmp_entry_read_bit_rack(wmp_entry);
      if (bit_rack_get_letter(&word_bit_rack, BLANK_MACHINE_LETTER) != 0) {
        continue;
      }
      uint8_t word_letter_counts[BIT_RACK_MAX_ALPHABET_SIZE];
      for (int ml = 0; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
        word_letter_counts[ml] =
            (uint8_t)bit_rack_get_letter(&word_bit_rack, ml);
      }

      for (int cfg_idx = 0; cfg_idx < NUM_MULTI_PT_TP_CONFIGS; cfg_idx++) {
        const MultiPtTpConfig *cfg = &MULTI_PT_TP_CONFIGS[cfg_idx];
        if (word_length < cfg->min_word_length ||
            word_length > cfg->max_word_length) {
          continue;
        }
        SubmultisetWalkState state;
        state.word_letter_counts = word_letter_counts;
        memset(state.submultiset_counts, 0, sizeof(state.submultiset_counts));
        state.len_idx = word_length - cfg->min_word_length;
        state.temp_table = temp_tables[cfg_idx];
        submultiset_walk_recursive(&state, 0, cfg->subrack_size);
      }
    }
  }
}

typedef struct {
  const LetterDistribution *ld;
  int ld_size;
  const uint32_t *result; // size = num_word_lengths for this tp
  int num_word_lengths;
  int target_cfg_idx;
  const RackInfoTable *rit;
  RackInfoTableEntry *entries;
} SupersetWalkState;

// Phase 2 leaf action: we have a 7-tile rack `current_rack` (S +
// extra letters). Look it up in the main RIT and OR `result[k]` into
// the corresponding per-tp bitvec slot for every k.
static void superset_walk_leaf(const SupersetWalkState *state,
                               const BitRack *current_rack) {
  const RackInfoTableEntry *const_entry =
      rack_info_table_lookup(state->rit, current_rack);
  if (const_entry == NULL) {
    return;
  }
  const size_t entry_idx = (size_t)(const_entry - state->rit->entries);
  RackInfoTableEntry *entry = &state->entries[entry_idx];
  switch (state->target_cfg_idx) {
  case 0: // tp=6
    for (int k = 0; k < state->num_word_lengths; k++) {
      entry->multi_pt_tp6_bitvec[k] |= state->result[k];
    }
    break;
  case 1: // tp=7
    for (int k = 0; k < state->num_word_lengths; k++) {
      entry->multi_pt_tp7_bitvec[k] |= state->result[k];
    }
    break;
  default:
    break;
  }
}

// Recursively add `letters_to_add` letters from the alphabet to
// `current_rack` (which starts at the canonical k-tile S) to form 7-tile
// supersets, calling the leaf action at each completed 7-tile rack.
// Iterates letters in canonical (non-decreasing) order starting from
// `next_ml` to avoid double-counting unordered multisets. Respects the
// LD's per-letter cap.
static void superset_walk_recursive(const SupersetWalkState *state,
                                    BitRack *current_rack, int letters_to_add,
                                    int next_ml) {
  if (letters_to_add == 0) {
    superset_walk_leaf(state, current_rack);
    return;
  }
  // Skip the blank slot: the bitvec is only populated for blankless
  // racks.
  const int start = next_ml < 1 ? 1 : next_ml;
  for (int ml = start; ml < state->ld_size; ml++) {
    if ((int)bit_rack_get_letter(current_rack, ml) >=
        ld_get_dist(state->ld, ml)) {
      continue;
    }
    bit_rack_add_letter(current_rack, (MachineLetter)ml);
    superset_walk_recursive(state, current_rack, letters_to_add - 1, ml);
    bit_rack_take_letter(current_rack, (MachineLetter)ml);
  }
}

// Phase 2: walk one temp table and distribute its accumulated results
// to the main RIT entries via the (RACK_SIZE - subrack_size)-letter
// superset enumeration.
static void phase2_distribute_temp_table(const TempSubrackTable *temp_table,
                                         int cfg_idx,
                                         const LetterDistribution *ld,
                                         const RackInfoTable *rit,
                                         RackInfoTableEntry *entries) {
  const MultiPtTpConfig *cfg = &MULTI_PT_TP_CONFIGS[cfg_idx];
  const int letters_to_add = RACK_SIZE - cfg->subrack_size;
  for (uint32_t i = 0; i < temp_table->num_entries; i++) {
    const TempSubrackEntry *temp_entry = &temp_table->entries[i];
    // Skip entries with no bits set in any slot -- nothing to distribute.
    uint32_t any = 0;
    for (int k = 0; k < cfg->num_word_lengths; k++) {
      any |= temp_entry->result[k];
    }
    if (any == 0) {
      continue;
    }
    SupersetWalkState state;
    state.ld = ld;
    state.ld_size = ld_get_size(ld);
    state.result = temp_entry->result;
    state.num_word_lengths = cfg->num_word_lengths;
    state.target_cfg_idx = cfg_idx;
    state.rit = rit;
    state.entries = entries;
    BitRack current = temp_entry->subrack;
    superset_walk_recursive(&state, &current, letters_to_add, 0);
  }
}

static void populate_multi_pt_bitvecs(const WMP *wmp,
                                      const LetterDistribution *ld,
                                      const RackInfoTable *rit,
                                      RackInfoTableEntry *entries) {
  TempSubrackTable *temp_tables[NUM_MULTI_PT_TP_CONFIGS];
  for (int cfg_idx = 0; cfg_idx < NUM_MULTI_PT_TP_CONFIGS; cfg_idx++) {
    const MultiPtTpConfig *cfg = &MULTI_PT_TP_CONFIGS[cfg_idx];
    temp_tables[cfg_idx] = temp_subrack_table_create(ld, cfg->subrack_size);
  }
  phase1_accumulate_temp_tables(wmp, temp_tables);
  for (int cfg_idx = 0; cfg_idx < NUM_MULTI_PT_TP_CONFIGS; cfg_idx++) {
    phase2_distribute_temp_table(temp_tables[cfg_idx], cfg_idx, ld, rit,
                                 entries);
    temp_subrack_table_destroy(temp_tables[cfg_idx]);
  }
}

// ============================================================================
// Thread work for parallel entry computation
// ============================================================================

typedef struct {
  const KLV *klv;
  const WMP *wmp;
  const LetterDistribution *ld;
  uint8_t playthrough_min_played_size;
  const BitRack *all_racks;
  RackInfoTableEntry *entries;
  const uint32_t *entry_indices;
  uint32_t start;
  uint32_t end;
} EntryComputeArg;

static void *compute_entries_thread(void *arg) {
  const EntryComputeArg *a = (const EntryComputeArg *)arg;
  for (uint32_t rack_idx = a->start; rack_idx < a->end; rack_idx++) {
    const uint32_t entry_idx = a->entry_indices[rack_idx];
    RackInfoTableEntry *entry = &a->entries[entry_idx];
    compute_entry_for_rack(a->klv, a->wmp, a->ld,
                           a->playthrough_min_played_size,
                           &a->all_racks[rack_idx], entry);
  }
  return NULL;
}

// ============================================================================
// Main construction function
// ============================================================================

RackInfoTable *make_rack_info_table(const KLV *klv, const WMP *wmp,
                                    const LetterDistribution *ld,
                                    int num_threads,
                                    uint8_t playthrough_min_played_size) {
  if (num_threads <= 0) {
    num_threads = 1;
  }

  // If the WMP is NULL, or the caller asked for no coverage, clamp the
  // coverage to empty so no playthrough work runs.
  uint8_t effective_min = playthrough_min_played_size;
  if (wmp == NULL || effective_min > RACK_SIZE) {
    effective_min = (uint8_t)(RACK_SIZE + 1);
  }

  // 1. Count total racks
  const uint32_t total_racks = count_all_racks(ld);
  if (total_racks == 0) {
    RackInfoTable *rit =
        (RackInfoTable *)calloc_or_die(1, sizeof(RackInfoTable));
    rit->version = RIT_VERSION;
    rit->rack_size = (uint8_t)RACK_SIZE;
    rit->playthrough_min_played_size = effective_min;
    rit->num_buckets = RIT_MIN_BUCKETS;
    rit->num_entries = 0;
    rit->bucket_starts =
        (uint32_t *)calloc_or_die(RIT_MIN_BUCKETS + 1, sizeof(uint32_t));
    rit->entries = NULL;
    rit->name = NULL;
    return rit;
  }

  // 2. Enumerate all racks into a temporary array
  BitRack *all_racks = malloc_or_die((size_t)total_racks * sizeof(BitRack));
  BitRack current = bit_rack_create_empty();
  uint32_t fill_index = 0;
  enumerate_racks_recursive(ld, ld_get_size(ld), 0, RACK_SIZE, &current,
                            all_racks, &fill_index);

  // 3. Determine bucket count and count per bucket
  uint32_t num_buckets = next_power_of_2(total_racks);
  if (num_buckets < RIT_MIN_BUCKETS) {
    num_buckets = RIT_MIN_BUCKETS;
  }

  uint32_t *bucket_counts = calloc_or_die(num_buckets, sizeof(uint32_t));
  for (uint32_t rack_idx = 0; rack_idx < total_racks; rack_idx++) {
    const uint32_t bucket =
        bit_rack_get_bucket_index(&all_racks[rack_idx], num_buckets);
    bucket_counts[bucket]++;
  }

  // 4. Build bucket_starts via prefix sum
  uint32_t *bucket_starts = malloc_or_die((num_buckets + 1) * sizeof(uint32_t));
  uint32_t offset = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < num_buckets; bucket_idx++) {
    bucket_starts[bucket_idx] = offset;
    offset += bucket_counts[bucket_idx];
  }
  bucket_starts[num_buckets] = offset;

  // 5. Allocate entries and assign racks to buckets
  RackInfoTableEntry *entries =
      malloc_or_die((size_t)total_racks * sizeof(RackInfoTableEntry));

  uint32_t *entry_indices = malloc_or_die(total_racks * sizeof(uint32_t));
  memset(bucket_counts, 0, num_buckets * sizeof(uint32_t));

  for (uint32_t rack_idx = 0; rack_idx < total_racks; rack_idx++) {
    const uint32_t bucket =
        bit_rack_get_bucket_index(&all_racks[rack_idx], num_buckets);
    const uint32_t entry_idx = bucket_starts[bucket] + bucket_counts[bucket]++;
    entry_indices[rack_idx] = entry_idx;
    rack_info_table_entry_write_bit_rack(&entries[entry_idx],
                                         &all_racks[rack_idx]);
  }

  free(bucket_counts);

  // 6. Compute per-entry data (parallel)
  if (num_threads == 1 || total_racks < (uint32_t)num_threads) {
    EntryComputeArg arg = {
        .klv = klv,
        .wmp = wmp,
        .ld = ld,
        .playthrough_min_played_size = effective_min,
        .all_racks = all_racks,
        .entries = entries,
        .entry_indices = entry_indices,
        .start = 0,
        .end = total_racks,
    };
    compute_entries_thread(&arg);
  } else {
    cpthread_t *threads =
        malloc_or_die((size_t)num_threads * sizeof(cpthread_t));
    EntryComputeArg *args =
        malloc_or_die((size_t)num_threads * sizeof(EntryComputeArg));

    const uint32_t chunk = total_racks / (uint32_t)num_threads;
    uint32_t remainder = total_racks % (uint32_t)num_threads;

    uint32_t start = 0;
    for (int thread_idx = 0; thread_idx < num_threads; thread_idx++) {
      uint32_t this_chunk = chunk + (remainder > 0 ? 1 : 0);
      if (remainder > 0) {
        remainder--;
      }
      args[thread_idx] = (EntryComputeArg){
          .klv = klv,
          .wmp = wmp,
          .ld = ld,
          .playthrough_min_played_size = effective_min,
          .all_racks = all_racks,
          .entries = entries,
          .entry_indices = entry_indices,
          .start = start,
          .end = start + this_chunk,
      };
      cpthread_create(&threads[thread_idx], compute_entries_thread,
                      &args[thread_idx]);
      start += this_chunk;
    }

    for (int thread_idx = 0; thread_idx < num_threads; thread_idx++) {
      cpthread_join(threads[thread_idx]);
    }

    free(threads);
    free(args);
  }

  free(entry_indices);
  free(all_racks);

  // 7. Build the RackInfoTable
  RackInfoTable *rit = (RackInfoTable *)calloc_or_die(1, sizeof(RackInfoTable));
  rit->version = RIT_VERSION;
  rit->rack_size = (uint8_t)RACK_SIZE;
  rit->playthrough_min_played_size = effective_min;
  rit->num_buckets = num_buckets;
  rit->num_entries = total_racks;
  rit->bucket_starts = bucket_starts;
  rit->entries = entries;
  rit->name = NULL;

  // 8. Populate the multi-playthrough bitvecs by flipping the walk:
  // iterate WMP blankless words of length 6..15 and OR bits into the RIT
  // entry for every canonical 5-, 6-, and 7-tile submultiset of each
  // word. Requires rit to be a valid lookup target, which is why this
  // runs after step 7.
  if (wmp != NULL) {
    populate_multi_pt_bitvecs(wmp, ld, rit, entries);
  }

  return rit;
}
