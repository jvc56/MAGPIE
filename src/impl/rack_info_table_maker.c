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
      state->entry->leaves[state->leave_map->current_index] = value;

      if (state->wmp != NULL) {
        // Build the BitRack for the played subrack (state->player_rack
        // holds the "would be played" side at this leaf; state->leave
        // holds the complement "would be the leave after playing it").
        const BitRack played_bit_rack =
            bit_rack_create_from_rack(state->ld, state->player_rack);
        const int leave_size = rit_maker_popcount(
            (unsigned int)state->leave_map->current_index);

        // Nonplaythrough existence cache: does this specific size-
        // played_size subrack form a valid played_size-letter word on its
        // own? If so, mark the corresponding bit in
        // nonplaythrough_has_word_of_length_bitmask and track the best
        // leave value over all subracks that satisfy this property.
        // Replaces the per-movegen wmp_move_gen_check_nonplaythrough_
        // existence warmup that otherwise runs C(RACK_SIZE, k) WMP
        // lookups per size k on every movegen call.
        if (played_size >= MINIMUM_WORD_LENGTH && played_size <= BOARD_DIM) {
          const WMPEntry *np_wmp_entry = wmp_get_word_entry(
              state->wmp, &played_bit_rack, played_size);
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
            const int cap = state->ld_size < (int)BIT_RACK_MAX_ALPHABET_SIZE
                                ? state->ld_size
                                : (int)BIT_RACK_MAX_ALPHABET_SIZE;
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

  memset(entry->leaves, 0,
         RACK_INFO_TABLE_LEAVES_PER_ENTRY * sizeof(Equity));
  memset(entry->playthrough_union, 0,
         RACK_INFO_TABLE_UNIONS_PER_ENTRY * sizeof(uint32_t));
  entry->nonplaythrough_has_word_of_length_bitmask = 0;
  memset(entry->pad, 0, sizeof(entry->pad));
  for (int leave_idx = 0;
       leave_idx < RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY;
       leave_idx++) {
    entry->nonplaythrough_best_leave_values[leave_idx] = EQUITY_INITIAL_VALUE;
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
  };

  const uint32_t root = kwg_get_dawg_root_node_index(klv_get_kwg(klv));
  compute_entry_recursive(&state, root, 0, 0);
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
  uint32_t *bucket_starts =
      malloc_or_die((num_buckets + 1) * sizeof(uint32_t));
  uint32_t offset = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < num_buckets; bucket_idx++) {
    bucket_starts[bucket_idx] = offset;
    offset += bucket_counts[bucket_idx];
  }
  bucket_starts[num_buckets] = offset;

  // 5. Allocate entries and assign racks to buckets
  RackInfoTableEntry *entries =
      malloc_or_die((size_t)total_racks * sizeof(RackInfoTableEntry));

  uint32_t *entry_indices =
      malloc_or_die(total_racks * sizeof(uint32_t));
  memset(bucket_counts, 0, num_buckets * sizeof(uint32_t));

  for (uint32_t rack_idx = 0; rack_idx < total_racks; rack_idx++) {
    const uint32_t bucket =
        bit_rack_get_bucket_index(&all_racks[rack_idx], num_buckets);
    const uint32_t entry_idx =
        bucket_starts[bucket] + bucket_counts[bucket]++;
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
  RackInfoTable *rit =
      (RackInfoTable *)calloc_or_die(1, sizeof(RackInfoTable));
  rit->version = RIT_VERSION;
  rit->rack_size = (uint8_t)RACK_SIZE;
  rit->playthrough_min_played_size = effective_min;
  rit->num_buckets = num_buckets;
  rit->num_entries = total_racks;
  rit->bucket_starts = bucket_starts;
  rit->entries = entries;
  rit->name = NULL;

  return rit;
}
