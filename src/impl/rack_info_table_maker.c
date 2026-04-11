#include "rack_info_table_maker.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/klv_defs.h"
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
// Leave value computation
//
// For a given rack, compute leave values for all 2^RACK_SIZE subsets using
// the same recursive enumeration as generate_exchange_moves in move_gen.c.
// The complement index maps subsets to the same bitmask positions used by
// LeaveMap during play generation.
// ============================================================================

static void compute_leaves_recursive(const KLV *klv, int ld_size,
                                     Rack *player_rack, Rack *leave,
                                     LeaveMap *leave_map, Equity *out_leaves,
                                     uint32_t node_index, uint32_t word_index,
                                     MachineLetter ml) {
  while (ml < ld_size && rack_get_letter(player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    if (rack_get_total_letters(player_rack) > 0) {
      Equity value = 0;
      if (word_index != KLV_UNFOUND_INDEX) {
        value = klv_get_indexed_leave_value(klv, word_index - 1);
      }
      out_leaves[leave_map->current_index] = value;
    }
    return;
  }

  compute_leaves_recursive(klv, ld_size, player_rack, leave, leave_map,
                           out_leaves, node_index, word_index, ml + 1);

  const uint16_t num_this = rack_get_letter(player_rack, ml);
  for (uint16_t tile_idx = 0; tile_idx < num_this; tile_idx++) {
    rack_add_letter(leave, ml);
    leave_map_take_letter_and_update_complement_index(leave_map, player_rack,
                                                      ml);
    uint32_t sibling_word_index;
    node_index = increment_node_to_ml(klv, node_index, word_index,
                                      &sibling_word_index, ml);
    word_index = sibling_word_index;
    uint32_t child_word_index;
    node_index = follow_arc(klv, node_index, word_index, &child_word_index);
    word_index = child_word_index;
    compute_leaves_recursive(klv, ld_size, player_rack, leave, leave_map,
                             out_leaves, node_index, word_index, ml + 1);
  }

  rack_take_letters(leave, ml, num_this);
  for (int tile_idx = 0; tile_idx < num_this; tile_idx++) {
    leave_map_add_letter_and_update_complement_index(leave_map, player_rack,
                                                     ml);
  }
}

static void compute_entry_for_rack(const KLV *klv,
                                   const LetterDistribution *ld,
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

  const uint32_t root = kwg_get_dawg_root_node_index(klv_get_kwg(klv));
  compute_leaves_recursive(klv, ld_size, &player_rack, &leave, &leave_map,
                           entry->leaves, root, 0, 0);
}

// ============================================================================
// Thread work for parallel entry computation
// ============================================================================

typedef struct {
  const KLV *klv;
  const WMP *wmp; // Reserved for future per-rack data
  const LetterDistribution *ld;
  const BitRack *all_racks;
  RackInfoTableEntry *entries;
  const uint32_t *entry_indices;
  uint32_t start;
  uint32_t end;
} EntryComputeArg;

static void *compute_entries_thread(void *arg) {
  const EntryComputeArg *a = (const EntryComputeArg *)arg;
  (void)a->wmp; // unused for now
  for (uint32_t rack_idx = a->start; rack_idx < a->end; rack_idx++) {
    const uint32_t entry_idx = a->entry_indices[rack_idx];
    RackInfoTableEntry *entry = &a->entries[entry_idx];
    compute_entry_for_rack(a->klv, a->ld, &a->all_racks[rack_idx], entry);
  }
  return NULL;
}

// ============================================================================
// Main construction function
// ============================================================================

RackInfoTable *make_rack_info_table(const KLV *klv, const WMP *wmp,
                                    const LetterDistribution *ld,
                                    int num_threads) {
  if (num_threads <= 0) {
    num_threads = 1;
  }

  // 1. Count total racks
  const uint32_t total_racks = count_all_racks(ld);
  if (total_racks == 0) {
    RackInfoTable *rit =
        (RackInfoTable *)calloc_or_die(1, sizeof(RackInfoTable));
    rit->version = RIT_VERSION;
    rit->rack_size = (uint8_t)RACK_SIZE;
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
  rit->num_buckets = num_buckets;
  rit->num_entries = total_racks;
  rit->bucket_starts = bucket_starts;
  rit->entries = entries;
  rit->name = NULL;

  return rit;
}
