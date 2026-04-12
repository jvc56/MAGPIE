#ifndef RACK_INFO_TABLE_H
#define RACK_INFO_TABLE_H

#include "../compat/endian_conv.h"
#include "../def/bit_rack_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/equity.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A RackInfoTable maps full-rack BitRacks to per-rack data computed ahead of
// time. The table is keyed by BitRack (the multiset of tiles forming a full
// rack) and uses the same bucket-chaining hash table layout as the WordMap.
//
// Each entry stores leave values for every 2^RACK_SIZE subset of the rack,
// indexed by the LeaveMap bitmask (empty at index 0, full rack at index
// (1 << RACK_SIZE) - 1).
//
// Each entry also stores a fixed-size array `playthrough_union[leave_size]`
// indexed by leave_size in [0, RACK_SIZE]. Each uint32 is a union bitmask:
//
//   playthrough_union[leave_size] = bitmask of letters L such that there
//       exists some canonical (RACK_SIZE - leave_size)-tile subrack S of
//       the full rack for which (S + {L}) anagrams to a valid word of
//       length (RACK_SIZE - leave_size + 1).
//
// This is the OR-reduction of per-canonical-subrack "can a word of this
// length exist with one additional letter" checks. Shadow consumes it with
// a count of tiles_played, which maps to leave_size = RACK_SIZE -
// tiles_played, without needing to know which specific subrack is being
// played.
//
// The coverage interval [playthrough_min_played_size, RACK_SIZE] controls
// which leave_sizes are populated by the build. Uncovered entries stay at
// 0; consumers must check rack_info_table_has_playthrough_coverage() before
// trusting a zero result.
//
// Each entry also stores `nonplaythrough_has_word_of_length` (a uint8 bitmask
// indexed by word_length in [0, RACK_SIZE]) and
// `nonplaythrough_best_leave_values[leave_size]` (one Equity per leave_size
// in [0, RACK_SIZE]). These cache the result of the per-movegen warmup
// wmp_move_gen_check_nonplaythrough_existence used to precompute (a) which
// word lengths have SOME canonical size-k subrack of the rack that already
// forms a valid k-letter word and (b) the best KLV leave value among those
// subracks, so shadow can early-exit nonplaythrough anchors without running
// that warmup loop over all C(RACK_SIZE, k) canonical subracks per rack per
// movegen call.
//
// Together these are a superset of Other Claude's proposed "best leave per
// subset-size" field (#4 from his list) restricted to leaves reachable via
// an actual no-playthrough word -- which is exactly the quantity shadow
// compares against `gen->best_leaves` for nonplaythrough equity math.
//
// Finally each entry also stores three multi-playthrough bitvecs, one for
// each tiles_played value in {5, 6, 7}, indexed by word length and bit-
// per-letter:
//
//   multi_pt_tp{5,6,7}_bitvec[length_idx]
//
// The bitvec for tp=N covers word lengths [N+1, BOARD_DIM]. Bit L of slot
// k is set iff there exists some word of length (N+1+k) whose multiset
// equals (some N-tile subrack of this rack) + {L} + (word_length - N - 1)
// other letters.
//
// Shadow uses these to prune multi-playthrough anchors before the existing
// wmp hash-walk fallback (or, for partial-rack cases that previously had
// no check at all, as the only word-existence check). Loading the single
// uint32 for the target word length and testing each playthrough letter's
// bit is much cheaper than the wmp_get_word_entry-based fallback.
//
// The tp=7 bitvec is consumed in the full-rack bingo case (tiles_played
// == RACK_SIZE) as a pre-filter ahead of the existing wmp walk. The tp=5
// and tp=6 bitvecs are consumed in the partial-rack multi-playthrough
// case (tiles_played < RACK_SIZE) which had no shadow-time check at all
// before this. Together they cover all multi-playthrough plays where
// tiles_played is in {5, 6, 7}, which is the bulk of the multi-pt
// shadow_record bypass volume.
//
// All three bitvecs are only populated for racks with zero blanks; blank
// racks leave them at 0 and the consumer skips the check for them,
// falling through to whatever the prior path was.
//
// Indexing the array by word length (rather than by letter) keeps the
// inner check loop O(num_playthrough_tiles) of register-only
// `(length_bitvec >> ml) & 1` tests after a single uint32 load.

enum {
  RACK_INFO_TABLE_LEAVES_PER_ENTRY = (1 << RACK_SIZE),
  RACK_INFO_TABLE_UNIONS_PER_ENTRY = (RACK_SIZE + 1),
  RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY = (RACK_SIZE + 1),
  RACK_INFO_TABLE_BITRACK_BYTES = BIT_RACK_EXPECTED_SIZE_BYTES,
  // Multi-playthrough bitvec ranges. For tp=N, the bitvec covers word
  // lengths [N + 1, BOARD_DIM] (slot 0 = single-pt, slot 1+ = multi-pt).
  // Slot 0 is technically redundant with playthrough_union for the
  // num_pt=1 case but we keep it for layout uniformity.
  RIT_MULTI_PT_TP7_MIN_WORD_LENGTH = RACK_SIZE + 1,
  RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS = 8,
  RIT_MULTI_PT_TP7_MAX_WORD_LENGTH =
      RIT_MULTI_PT_TP7_MIN_WORD_LENGTH + RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS - 1,
  RIT_MULTI_PT_TP6_MIN_WORD_LENGTH = (RACK_SIZE - 1) + 1,
  RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS = 9,
  RIT_MULTI_PT_TP6_MAX_WORD_LENGTH =
      RIT_MULTI_PT_TP6_MIN_WORD_LENGTH + RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS - 1,
  RIT_MULTI_PT_TP5_MIN_WORD_LENGTH = (RACK_SIZE - 2) + 1,
  RIT_MULTI_PT_TP5_NUM_WORD_LENGTHS = 10,
  RIT_MULTI_PT_TP5_MAX_WORD_LENGTH =
      RIT_MULTI_PT_TP5_MIN_WORD_LENGTH + RIT_MULTI_PT_TP5_NUM_WORD_LENGTHS - 1,
  // Bump RIT_VERSION whenever the on-disk layout changes incompatibly.
  RIT_VERSION = 7,
  RIT_EARLIEST_SUPPORTED_VERSION = 7,
};

typedef struct RackInfoTableEntry {
  Equity leaves[RACK_INFO_TABLE_LEAVES_PER_ENTRY];
  uint32_t playthrough_union[RACK_INFO_TABLE_UNIONS_PER_ENTRY];
  Equity nonplaythrough_best_leave_values
      [RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY];
  uint32_t multi_pt_tp7_bitvec[RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS];
  uint32_t multi_pt_tp6_bitvec[RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS];
  uint32_t multi_pt_tp5_bitvec[RIT_MULTI_PT_TP5_NUM_WORD_LENGTHS];
  uint8_t nonplaythrough_has_word_of_length_bitmask;
  uint8_t pad[3];
  uint8_t bit_rack_bytes[RACK_INFO_TABLE_BITRACK_BYTES];
} RackInfoTableEntry;

typedef struct RackInfoTable {
  char *name;
  uint8_t version;
  uint8_t rack_size;
  // Coverage lower bound for playthrough_union. Played sizes in
  // [playthrough_min_played_size, RACK_SIZE] have their corresponding
  // playthrough_union[leave_size] slot populated; other slots stay at 0.
  // Setting min > RACK_SIZE means "no coverage at all".
  uint8_t playthrough_min_played_size;
  uint32_t num_buckets;
  uint32_t num_entries;
  uint32_t *bucket_starts;
  RackInfoTableEntry *entries;
} RackInfoTable;

static inline const char *rack_info_table_get_name(const RackInfoTable *rit) {
  return rit->name;
}

static inline uint8_t
rack_info_table_get_playthrough_min_played_size(const RackInfoTable *rit) {
  return rit->playthrough_min_played_size;
}

// True if the given played_size falls within the RIT's coverage interval,
// i.e. playthrough_union[RACK_SIZE - played_size] is populated for the
// current rack.
static inline bool
rack_info_table_has_playthrough_coverage(const RackInfoTable *rit,
                                         int played_size) {
  if (played_size < 0 || played_size > RACK_SIZE) {
    return false;
  }
  return played_size >= (int)rit->playthrough_min_played_size;
}

static inline BitRack
rack_info_table_entry_read_bit_rack(const RackInfoTableEntry *entry) {
#if IS_LITTLE_ENDIAN
  BitRack bit_rack;
  memcpy(&bit_rack, entry->bit_rack_bytes, RACK_INFO_TABLE_BITRACK_BYTES);
  return bit_rack;
#else
  uint64_t low, high;
  memcpy(&low, entry->bit_rack_bytes, 8);
  memcpy(&high, entry->bit_rack_bytes + 8, 8);
  low = le64toh(low);
  high = le64toh(high);
  return (BitRack){.low = low, .high = high};
#endif
}

static inline void
rack_info_table_entry_write_bit_rack(RackInfoTableEntry *entry,
                                     const BitRack *bit_rack) {
#if IS_LITTLE_ENDIAN
  memcpy(entry->bit_rack_bytes, bit_rack, RACK_INFO_TABLE_BITRACK_BYTES);
#else
  uint64_t low = bit_rack_get_low_64(bit_rack);
  uint64_t high = bit_rack_get_high_64(bit_rack);
  low = htole64(low);
  high = htole64(high);
  memcpy(entry->bit_rack_bytes, &low, 8);
  memcpy(entry->bit_rack_bytes + 8, &high, 8);
#endif
}

// Look up the entry for a full rack. Returns NULL if not found.
static inline const RackInfoTableEntry *
rack_info_table_lookup(const RackInfoTable *rit, const BitRack *bit_rack) {
  const uint32_t bucket_idx =
      bit_rack_get_bucket_index(bit_rack, rit->num_buckets);
  const uint32_t start = rit->bucket_starts[bucket_idx];
  const uint32_t end = rit->bucket_starts[bucket_idx + 1];
  for (uint32_t entry_idx = start; entry_idx < end; entry_idx++) {
    const RackInfoTableEntry *entry = &rit->entries[entry_idx];
    const BitRack entry_bit_rack = rack_info_table_entry_read_bit_rack(entry);
    if (bit_rack_equals(&entry_bit_rack, bit_rack)) {
      return entry;
    }
  }
  return NULL;
}

// Convenience accessor: return the leaves array for a rack, or NULL.
static inline const Equity *
rack_info_table_lookup_leaves(const RackInfoTable *rit,
                              const BitRack *bit_rack) {
  const RackInfoTableEntry *entry = rack_info_table_lookup(rit, bit_rack);
  if (entry == NULL) {
    return NULL;
  }
  return entry->leaves;
}

// Bitmask of machine letters L such that, for at least one canonical
// (RACK_SIZE - leave_size)-tile subrack S of the full rack represented by
// `entry`, S + {L} anagrams to a valid word of length
// (RACK_SIZE - leave_size + 1). Returns 0 if leave_size is out of range.
// Callers should separately verify via
// rack_info_table_has_playthrough_coverage() that the table actually has data
// for the corresponding played_size -- a 0 value here alone can't distinguish
// "no letter works" from "we didn't build coverage for this size".
static inline uint32_t
rack_info_table_entry_get_playthrough_union(const RackInfoTableEntry *entry,
                                            int leave_size) {
  if (leave_size < 0 || leave_size > RACK_SIZE) {
    return 0;
  }
  return entry->playthrough_union[leave_size];
}

// Returns true if there is at least one canonical size-`word_length` subrack
// of this rack that forms a valid `word_length`-letter word on its own (no
// playthrough). Shadow uses this to skip anchors whose tiles_played size
// could never produce a nonplaythrough word for this rack.
static inline bool rack_info_table_entry_has_nonplaythrough_word_of_length(
    const RackInfoTableEntry *entry, int word_length) {
  if (word_length < 0 || word_length > RACK_SIZE) {
    return false;
  }
  return ((entry->nonplaythrough_has_word_of_length_bitmask >> word_length) &
          1U) != 0U;
}

// Returns the max KLV leave value among canonical (RACK_SIZE - leave_size)-
// tile subracks of this rack that form a valid (RACK_SIZE - leave_size)-
// letter word on their own (no playthrough). Returns EQUITY_INITIAL_VALUE if
// no such subrack exists for this leave_size. Shadow uses this as the best
// possible leave for a nonplaythrough play of size (RACK_SIZE - leave_size).
static inline Equity rack_info_table_entry_get_nonplaythrough_best_leave_value(
    const RackInfoTableEntry *entry, int leave_size) {
  if (leave_size < 0 || leave_size > RACK_SIZE) {
    return EQUITY_INITIAL_VALUE;
  }
  return entry->nonplaythrough_best_leave_values[leave_size];
}

// Returns the tp=N multi-playthrough bitvec uint32 for `word_length`. Bit
// L of the returned value is set iff there exists some word of `word_length`
// letters whose multiset equals (some N-tile subrack of this rack) + {L} +
// (word_length - N - 1) other letters. Returns 0 for out-of-range
// word_length (which reads as "no letter works" but the caller should not
// prune on 0 without first checking that the rack is blankless -- see the
// header comment on RackInfoTableEntry).
static inline uint32_t
rack_info_table_entry_get_multi_pt_tp7_bitvec(const RackInfoTableEntry *entry,
                                              int word_length) {
  if (word_length < RIT_MULTI_PT_TP7_MIN_WORD_LENGTH ||
      word_length > RIT_MULTI_PT_TP7_MAX_WORD_LENGTH) {
    return 0;
  }
  return entry
      ->multi_pt_tp7_bitvec[word_length - RIT_MULTI_PT_TP7_MIN_WORD_LENGTH];
}

static inline uint32_t
rack_info_table_entry_get_multi_pt_tp6_bitvec(const RackInfoTableEntry *entry,
                                              int word_length) {
  if (word_length < RIT_MULTI_PT_TP6_MIN_WORD_LENGTH ||
      word_length > RIT_MULTI_PT_TP6_MAX_WORD_LENGTH) {
    return 0;
  }
  return entry
      ->multi_pt_tp6_bitvec[word_length - RIT_MULTI_PT_TP6_MIN_WORD_LENGTH];
}

static inline uint32_t
rack_info_table_entry_get_multi_pt_tp5_bitvec(const RackInfoTableEntry *entry,
                                              int word_length) {
  if (word_length < RIT_MULTI_PT_TP5_MIN_WORD_LENGTH ||
      word_length > RIT_MULTI_PT_TP5_MAX_WORD_LENGTH) {
    return 0;
  }
  return entry
      ->multi_pt_tp5_bitvec[word_length - RIT_MULTI_PT_TP5_MIN_WORD_LENGTH];
}

static inline void rack_info_table_destroy(RackInfoTable *rit) {
  if (!rit) {
    return;
  }
  free(rit->name);
  free(rit->bucket_starts);
  free(rit->entries);
  free(rit);
}

// ============================================================================
// File I/O
// ============================================================================
//
// On-disk layout (all integers little-endian):
//   1 byte:  version (RIT_VERSION)
//   1 byte:  rack_size (matches RACK_SIZE)
//   1 byte:  playthrough_min_played_size
//   1 byte:  zero padding (keeps the following u32 aligned)
//   4 bytes: num_buckets
//   4 bytes: num_entries
//   (num_buckets + 1) * 4 bytes: bucket_starts
//   num_entries entries, each sizeof(RackInfoTableEntry) bytes:
//     (1 << RACK_SIZE) * 4 bytes: leaves (Equity, int32_t)
//     (RACK_SIZE + 1) * 4 bytes: playthrough_union (uint32_t)
//     (RACK_SIZE + 1) * 4 bytes: nonplaythrough_best_leave_values (Equity)
//     RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS * 4 bytes: multi_pt_tp7_bitvec
//                                                  (uint32 per word length)
//     RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS * 4 bytes: multi_pt_tp6_bitvec
//     RIT_MULTI_PT_TP5_NUM_WORD_LENGTHS * 4 bytes: multi_pt_tp5_bitvec
//     1 byte: nonplaythrough_has_word_of_length_bitmask
//     3 bytes: pad
//     16 bytes: bit_rack_bytes (full BitRack for collision check)

static inline void rit_write_uint32_or_die(uint32_t value, FILE *stream,
                                           const char *description) {
  const uint32_t le = htole32(value);
  fwrite_or_die(&le, sizeof(le), 1, stream, description);
}

static inline void rit_write_uint32s_or_die(const uint32_t *values, size_t n,
                                            FILE *stream,
                                            const char *description) {
#if IS_LITTLE_ENDIAN
  fwrite_or_die(values, sizeof(uint32_t), n, stream, description);
#else
  for (size_t i = 0; i < n; i++) {
    const uint32_t le = htole32(values[i]);
    fwrite_or_die(&le, sizeof(uint32_t), 1, stream, description);
  }
#endif
}

static inline void rit_write_entries_or_die(const RackInfoTableEntry *entries,
                                            uint32_t n, FILE *stream) {
#if IS_LITTLE_ENDIAN
  fwrite_or_die(entries, sizeof(RackInfoTableEntry), n, stream, "rit entries");
#else
  for (uint32_t i = 0; i < n; i++) {
    const RackInfoTableEntry *entry = &entries[i];
    for (int leaf_idx = 0; leaf_idx < RACK_INFO_TABLE_LEAVES_PER_ENTRY;
         leaf_idx++) {
      const uint32_t le_leaf = htole32((uint32_t)entry->leaves[leaf_idx]);
      fwrite_or_die(&le_leaf, sizeof(uint32_t), 1, stream, "rit leaf");
    }
    for (int union_idx = 0; union_idx < RACK_INFO_TABLE_UNIONS_PER_ENTRY;
         union_idx++) {
      const uint32_t le_union = htole32(entry->playthrough_union[union_idx]);
      fwrite_or_die(&le_union, sizeof(uint32_t), 1, stream,
                    "rit playthrough union");
    }
    for (int leave_idx = 0;
         leave_idx < RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY;
         leave_idx++) {
      const uint32_t le_leave =
          htole32((uint32_t)entry->nonplaythrough_best_leave_values[leave_idx]);
      fwrite_or_die(&le_leave, sizeof(uint32_t), 1, stream,
                    "rit nonplaythrough best leave value");
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS;
         len_idx++) {
      const uint32_t le_bitvec = htole32(entry->multi_pt_tp7_bitvec[len_idx]);
      fwrite_or_die(&le_bitvec, sizeof(uint32_t), 1, stream,
                    "rit multi pt tp7 bitvec");
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS;
         len_idx++) {
      const uint32_t le_bitvec = htole32(entry->multi_pt_tp6_bitvec[len_idx]);
      fwrite_or_die(&le_bitvec, sizeof(uint32_t), 1, stream,
                    "rit multi pt tp6 bitvec");
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP5_NUM_WORD_LENGTHS;
         len_idx++) {
      const uint32_t le_bitvec = htole32(entry->multi_pt_tp5_bitvec[len_idx]);
      fwrite_or_die(&le_bitvec, sizeof(uint32_t), 1, stream,
                    "rit multi pt tp5 bitvec");
    }
    fwrite_or_die(&entry->nonplaythrough_has_word_of_length_bitmask,
                  sizeof(uint8_t), 1, stream,
                  "rit nonplaythrough has word of length bitmask");
    fwrite_or_die(entry->pad, sizeof(uint8_t), sizeof(entry->pad), stream,
                  "rit entry pad");
    fwrite_or_die(entry->bit_rack_bytes, sizeof(uint8_t),
                  RACK_INFO_TABLE_BITRACK_BYTES, stream, "rit bit_rack_bytes");
  }
#endif
}

static inline void rack_info_table_write_to_file(const RackInfoTable *rit,
                                                 const char *filename,
                                                 ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const uint8_t version = rit->version;
  fwrite_or_die(&version, sizeof(version), 1, stream, "rit version");
  const uint8_t rack_size = rit->rack_size;
  fwrite_or_die(&rack_size, sizeof(rack_size), 1, stream, "rit rack size");
  const uint8_t min_played = rit->playthrough_min_played_size;
  fwrite_or_die(&min_played, sizeof(min_played), 1, stream,
                "rit playthrough min played size");
  const uint8_t padding = 0;
  fwrite_or_die(&padding, sizeof(padding), 1, stream, "rit header padding");

  rit_write_uint32_or_die(rit->num_buckets, stream, "rit num buckets");
  rit_write_uint32_or_die(rit->num_entries, stream, "rit num entries");
  rit_write_uint32s_or_die(rit->bucket_starts, (size_t)rit->num_buckets + 1,
                           stream, "rit bucket starts");
  rit_write_entries_or_die(rit->entries, rit->num_entries, stream);
  fclose_or_die(stream);
}

static inline void rit_read_uint32_or_die(uint32_t *out, FILE *stream) {
  if (fread(out, sizeof(uint32_t), 1, stream) != 1) {
    log_fatal("could not read uint32 from rit stream");
  }
  *out = le32toh(*out);
}

static inline void rit_read_uint32s_or_die(uint32_t *out, size_t n,
                                           FILE *stream) {
  if (fread(out, sizeof(uint32_t), n, stream) != n) {
    log_fatal("could not read uint32s from rit stream");
  }
#if !IS_LITTLE_ENDIAN
  for (size_t i = 0; i < n; i++) {
    out[i] = le32toh(out[i]);
  }
#endif
}

static inline void rit_read_entries_or_die(RackInfoTableEntry *entries,
                                           uint32_t n, FILE *stream) {
  if (fread(entries, sizeof(RackInfoTableEntry), n, stream) != n) {
    log_fatal("could not read entries from rit stream");
  }
#if !IS_LITTLE_ENDIAN
  for (uint32_t i = 0; i < n; i++) {
    for (int leaf_idx = 0; leaf_idx < RACK_INFO_TABLE_LEAVES_PER_ENTRY;
         leaf_idx++) {
      entries[i].leaves[leaf_idx] =
          (Equity)le32toh((uint32_t)entries[i].leaves[leaf_idx]);
    }
    for (int union_idx = 0; union_idx < RACK_INFO_TABLE_UNIONS_PER_ENTRY;
         union_idx++) {
      entries[i].playthrough_union[union_idx] =
          le32toh(entries[i].playthrough_union[union_idx]);
    }
    for (int leave_idx = 0;
         leave_idx < RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY;
         leave_idx++) {
      entries[i].nonplaythrough_best_leave_values[leave_idx] = (Equity)le32toh(
          (uint32_t)entries[i].nonplaythrough_best_leave_values[leave_idx]);
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS;
         len_idx++) {
      entries[i].multi_pt_tp7_bitvec[len_idx] =
          le32toh(entries[i].multi_pt_tp7_bitvec[len_idx]);
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS;
         len_idx++) {
      entries[i].multi_pt_tp6_bitvec[len_idx] =
          le32toh(entries[i].multi_pt_tp6_bitvec[len_idx]);
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP5_NUM_WORD_LENGTHS;
         len_idx++) {
      entries[i].multi_pt_tp5_bitvec[len_idx] =
          le32toh(entries[i].multi_pt_tp5_bitvec[len_idx]);
    }
    // bit_rack_bytes are already stored little-endian
    // nonplaythrough_has_word_of_length_bitmask is a single byte, no swap
  }
#endif
}

static inline void rack_info_table_load_from_stream(RackInfoTable *rit,
                                                    FILE *stream,
                                                    const char *filename,
                                                    ErrorStack *error_stack) {
  uint8_t version;
  if (fread(&version, sizeof(version), 1, stream) != 1) {
    log_fatal("could not read rit version");
  }
  if (version < RIT_EARLIEST_SUPPORTED_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_UNSUPPORTED_VERSION,
        get_formatted_string(
            "detected rit version %d but only %d or greater is supported: %s\n",
            version, RIT_EARLIEST_SUPPORTED_VERSION, filename));
    return;
  }
  rit->version = version;

  uint8_t rack_size;
  if (fread(&rack_size, sizeof(rack_size), 1, stream) != 1) {
    log_fatal("could not read rit rack size");
  }
  if (rack_size != RACK_SIZE) {
    error_stack_push(error_stack, ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM,
                     get_formatted_string(
                         "rit rack size %d does not match build rack size %d: "
                         "%s\n",
                         rack_size, RACK_SIZE, filename));
    return;
  }
  rit->rack_size = rack_size;

  uint8_t min_played;
  if (fread(&min_played, sizeof(min_played), 1, stream) != 1) {
    log_fatal("could not read rit playthrough min played size");
  }
  rit->playthrough_min_played_size = min_played;

  uint8_t padding;
  if (fread(&padding, sizeof(padding), 1, stream) != 1) {
    log_fatal("could not read rit header padding");
  }

  rit_read_uint32_or_die(&rit->num_buckets, stream);
  rit_read_uint32_or_die(&rit->num_entries, stream);

  rit->bucket_starts = (uint32_t *)malloc_or_die(
      ((size_t)rit->num_buckets + 1) * sizeof(uint32_t));
  rit_read_uint32s_or_die(rit->bucket_starts, (size_t)rit->num_buckets + 1,
                          stream);

  rit->entries = (RackInfoTableEntry *)malloc_or_die(
      (size_t)rit->num_entries * sizeof(RackInfoTableEntry));
  rit_read_entries_or_die(rit->entries, rit->num_entries, stream);
}

static inline void rack_info_table_load(RackInfoTable *rit, const char *name,
                                        const char *filename,
                                        ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  rack_info_table_load_from_stream(rit, stream, filename, error_stack);
  fclose_or_die(stream);
  if (error_stack_is_empty(error_stack)) {
    rit->name = string_duplicate(name);
  }
}

static inline RackInfoTable *rack_info_table_create(const char *data_paths,
                                                    const char *rit_name,
                                                    ErrorStack *error_stack) {
  char *rit_filename = data_filepaths_get_readable_filename(
      data_paths, rit_name, DATA_FILEPATH_TYPE_RACK_INFO_TABLE, error_stack);
  RackInfoTable *rit = NULL;
  if (error_stack_is_empty(error_stack)) {
    rit = (RackInfoTable *)calloc_or_die(1, sizeof(RackInfoTable));
    rack_info_table_load(rit, rit_name, rit_filename, error_stack);
  }
  free(rit_filename);
  if (!error_stack_is_empty(error_stack)) {
    rack_info_table_destroy(rit);
    rit = NULL;
  }
  return rit;
}

#endif
