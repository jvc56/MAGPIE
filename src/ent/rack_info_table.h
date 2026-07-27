#ifndef RACK_INFO_TABLE_H
#define RACK_INFO_TABLE_H

#include "../compat/clonefile_compat.h"
#include "../compat/endian_conv.h"
#include "../def/bit_rack_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/equity.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
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
// Finally each entry also stores two multi-playthrough bitvecs, one for
// each tiles_played value in {6, 7}, indexed by word length and bit-
// per-letter:
//
//   multi_pt_tp{6,7}_bitvec[length_idx]
//
// The bitvec for tp=N covers word lengths [N+1, BOARD_DIM]. Bit L of slot
// k is set iff there exists some word of length (N+1+k) whose multiset
// equals (some N-tile subrack of this rack) + {L} + (word_length - N - 1)
// other letters.
//
// Shadow uses these to prune multi-playthrough anchors before the existing
// wmp hash-walk fallback (or, for the tp=6 case that previously had no
// check at all, as the only word-existence check). Loading the single
// uint32 for the target word length and testing each playthrough letter's
// bit is much cheaper than the wmp_get_word_entry-based fallback.
//
// The tp=7 bitvec is consumed in the full-rack bingo case (tiles_played
// == RACK_SIZE) as a pre-filter ahead of the existing wmp walk. The tp=6
// bitvec is consumed in the partial-rack multi-playthrough case
// (tiles_played == RACK_SIZE - 1) which had no shadow-time check at all
// before this. tp=5 was measured and dropped: its prune rate (4.7%) is
// roughly a fifth of tp=6's (23.4%) and didn't cover its file-size cost.
//
// Both bitvecs are only populated for racks with zero blanks; blank
// racks leave them at 0 and the consumer skips the check for them,
// falling through to whatever the prior path was.
//
// Indexing the array by word length (rather than by letter) keeps the
// inner check loop O(num_playthrough_tiles) of register-only
// `(length_bitvec >> ml) & 1` tests after a single uint32 load.

enum {
  RACK_INFO_TABLE_LEAVES_PER_ENTRY = (1 << RACK_SIZE),
  RACK_INFO_TABLE_LEAVES_PACKED_BYTES = (RACK_INFO_TABLE_LEAVES_PER_ENTRY * 3),
  RACK_INFO_TABLE_LEAVE_SIGN_BIT = (1 << 23),
  RACK_INFO_TABLE_LEAVE_MIN = -(1 << 23),
  RACK_INFO_TABLE_LEAVE_MAX = (1 << 23) - 1,
  RACK_INFO_TABLE_UNIONS_PER_ENTRY = (RACK_SIZE + 1),
  RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY = (RACK_SIZE + 1),
  RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY = (RACK_SIZE + 1),
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
  // Bump RIT_VERSION whenever the on-disk layout changes incompatibly.
  RIT_VERSION = 13,
  RIT_EARLIEST_SUPPORTED_VERSION = 11,
  RIT_MAX_INLINE_BINGO_WORDS = 1,
  // v13's original contextual layout stores two out-of-line arrays. It is
  // readable for compatibility, but its extra random memory accesses cost
  // simulation throughput.
  RIT_FLAG_CONTEXT_CAPS = 1,
  // The dedicated KLV3 layout stores the same capped maxima in the entry's
  // existing best-leave fields. It cannot serve KLV2 leave pruning, but the
  // model-named RIT is loaded only for its matching KLV3.
  RIT_FLAG_CONTEXT_CAPS_INLINE = 2,
  // Contains only model-independent word facts. The hash table and rack keys
  // are unchanged, but entries use RackInfoTableWordEntry instead of the
  // 584-byte full layout. Context-dependent evaluators can therefore retain
  // every safe RIT word optimization without mapping unused KLV2 payload.
  RIT_FLAG_WORD_ONLY = 4,
  RIT_KNOWN_FLAGS =
      RIT_FLAG_CONTEXT_CAPS | RIT_FLAG_CONTEXT_CAPS_INLINE | RIT_FLAG_WORD_ONLY,
  RIT_METADATA_SIZE = 32,
};

typedef struct RackInfoTableWordEntry {
  uint32_t playthrough_union[RACK_INFO_TABLE_UNIONS_PER_ENTRY];
  uint32_t multi_pt_tp7_bitvec[RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS];
  uint32_t multi_pt_tp6_bitvec[RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS];
  uint8_t nonplaythrough_has_word_of_length_bitmask;
  uint8_t num_bingo_words;
  MachineLetter bingo_words[RIT_MAX_INLINE_BINGO_WORDS][RACK_SIZE];
  // Make the 16-byte BitRack end on a naturally aligned 128-byte entry.
  uint8_t pad[3];
  uint8_t bit_rack_bytes[RACK_INFO_TABLE_BITRACK_BYTES];
} RackInfoTableWordEntry;

typedef struct RackInfoTableEntry {
  // Leave values packed as 24-bit signed little-endian integers.
  // Unpacked to 32-bit Equity in a batch at load time.
  uint8_t leaves_packed[RACK_INFO_TABLE_LEAVES_PACKED_BYTES];
  uint32_t playthrough_union[RACK_INFO_TABLE_UNIONS_PER_ENTRY];
  Equity nonplaythrough_best_leave_values
      [RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY];
  // Per-leave-size maximum leave value across all canonical subsets.
  // best_leaves[leave_size] = max over all C(RACK_SIZE, leave_size)
  // canonical subsets of the leave value for that subset. Used to skip
  // the 2^RACK_SIZE exchange enumeration walk when only best_leaves
  // bounds are needed (no exchange recording).
  Equity best_leaves[RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY];
  uint32_t multi_pt_tp7_bitvec[RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS];
  uint32_t multi_pt_tp6_bitvec[RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS];
  uint8_t nonplaythrough_has_word_of_length_bitmask;
  // Inline bingo words (nonplaythrough only). Up to RIT_MAX_INLINE_BINGO_WORDS
  // words of RACK_SIZE letters stored directly. num_bingo_words = 0 means no
  // inline bingos (>RIT_MAX_INLINE_BINGO_WORDS anagrams — fall back to WMP).
  uint8_t num_bingo_words;
  MachineLetter bingo_words[RIT_MAX_INLINE_BINGO_WORDS][RACK_SIZE];
  // Best exchange move across all leave sizes, tiebroken by compare_moves.
  MachineLetter best_exchange_strip[RACK_SIZE];
  uint8_t best_exchange_tiles_exchanged;
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
  RackInfoTableWordEntry *word_entries;
  // Optional v13 overlay. The ordinary entry fields remain KLV2-exact so one
  // contextual RIT can still serve KLV2 players. These arrays contain
  // max(base KLV2 value + per-leave contextual cap), indexed
  // [entry_index][leave_size].
  Equity *context_capped_best_leaves;
  Equity *context_capped_nonplaythrough_best_leave_values;
  uint8_t flags;
  uint64_t base_klv_fingerprint;
  uint64_t context_klv_fingerprint;
  uint32_t context_cap_quantile_ppm;
  // mmap state: when is_mmapped is true, bucket_starts and entries point
  // into the mapped region and must not be free'd individually.
  bool is_mmapped;
  void *mmap_base;
  size_t mmap_size;
} RackInfoTable;

static inline bool rack_info_table_is_word_only(const RackInfoTable *rit) {
  return rit != NULL && (rit->flags & RIT_FLAG_WORD_ONLY) != 0;
}

static inline void
rack_info_table_copy_word_entry(RackInfoTableWordEntry *destination,
                                const RackInfoTableEntry *source) {
  memcpy(destination->playthrough_union, source->playthrough_union,
         sizeof(destination->playthrough_union));
  memcpy(destination->multi_pt_tp7_bitvec, source->multi_pt_tp7_bitvec,
         sizeof(destination->multi_pt_tp7_bitvec));
  memcpy(destination->multi_pt_tp6_bitvec, source->multi_pt_tp6_bitvec,
         sizeof(destination->multi_pt_tp6_bitvec));
  destination->nonplaythrough_has_word_of_length_bitmask =
      source->nonplaythrough_has_word_of_length_bitmask;
  destination->num_bingo_words = source->num_bingo_words;
  memcpy(destination->bingo_words, source->bingo_words,
         sizeof(destination->bingo_words));
  memset(destination->pad, 0, sizeof(destination->pad));
  memcpy(destination->bit_rack_bytes, source->bit_rack_bytes,
         sizeof(destination->bit_rack_bytes));
}

// Unpack all 128 packed 24-bit leave values to 32-bit Equity in a batch.
// Sign-extends from bit 23. Designed to be auto-vectorized.
static inline void
rack_info_table_entry_unpack_leaves(const RackInfoTableEntry *entry,
                                    Equity *dest) {
  const uint8_t *src = entry->leaves_packed;
  for (int i = 0; i < RACK_INFO_TABLE_LEAVES_PER_ENTRY; i++) {
    const int offset = i * 3;
    int32_t val = (int32_t)src[offset] | ((int32_t)src[offset + 1] << 8) |
                  ((int32_t)src[offset + 2] << 16);
    // Sign extend from bit 23 (branchless).
    val =
        (val ^ RACK_INFO_TABLE_LEAVE_SIGN_BIT) - RACK_INFO_TABLE_LEAVE_SIGN_BIT;
    dest[i] = (Equity)val;
  }
}

// Pack a single 32-bit Equity to 3 bytes in the packed leaves array.
static inline void rack_info_table_entry_pack_leave(RackInfoTableEntry *entry,
                                                    int index, Equity value) {
  const int offset = index * 3;
  const uint32_t uval = (uint32_t)value;
  entry->leaves_packed[offset] = (uint8_t)(uval & 0xFF);
  entry->leaves_packed[offset + 1] = (uint8_t)((uval >> 8) & 0xFF);
  entry->leaves_packed[offset + 2] = (uint8_t)((uval >> 16) & 0xFF);
}

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

static inline BitRack
rack_info_table_word_entry_read_bit_rack(const RackInfoTableWordEntry *entry) {
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
  if (rack_info_table_is_word_only(rit)) {
    return NULL;
  }
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

// Looks up model-independent word facts and copies them into caller-owned
// storage. For a full RIT, *full_entry_out also receives the matching entry so
// KLV2 callers can consume its leave payload. For a word-only RIT it is NULL.
// Copying 128 bytes once per per-thread cache miss keeps the hot shadow path
// independent of the on-disk entry layout.
static inline bool
rack_info_table_lookup_word_entry(const RackInfoTable *rit,
                                  const BitRack *bit_rack,
                                  RackInfoTableWordEntry *word_entry_out,
                                  const RackInfoTableEntry **full_entry_out) {
  *full_entry_out = NULL;
  const uint32_t bucket_idx =
      bit_rack_get_bucket_index(bit_rack, rit->num_buckets);
  const uint32_t start = rit->bucket_starts[bucket_idx];
  const uint32_t end = rit->bucket_starts[bucket_idx + 1];
  if (rack_info_table_is_word_only(rit)) {
    for (uint32_t entry_idx = start; entry_idx < end; entry_idx++) {
      const RackInfoTableWordEntry *entry = &rit->word_entries[entry_idx];
      const BitRack entry_bit_rack =
          rack_info_table_word_entry_read_bit_rack(entry);
      if (bit_rack_equals(&entry_bit_rack, bit_rack)) {
        *word_entry_out = *entry;
        return true;
      }
    }
    return false;
  }
  for (uint32_t entry_idx = start; entry_idx < end; entry_idx++) {
    const RackInfoTableEntry *entry = &rit->entries[entry_idx];
    const BitRack entry_bit_rack = rack_info_table_entry_read_bit_rack(entry);
    if (bit_rack_equals(&entry_bit_rack, bit_rack)) {
      rack_info_table_copy_word_entry(word_entry_out, entry);
      *full_entry_out = entry;
      return true;
    }
  }
  return false;
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

static inline uint32_t rack_info_table_word_entry_get_playthrough_union(
    const RackInfoTableWordEntry *entry, int leave_size) {
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

static inline bool rack_info_table_word_entry_has_nonplaythrough_word_of_length(
    const RackInfoTableWordEntry *entry, int word_length) {
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

// Returns the precomputed best_leaves array for this rack entry. Each
// best_leaves[leave_size] is the maximum leave value across all canonical
// C(RACK_SIZE, leave_size) subsets of the rack, for leave_size in
// [0, RACK_SIZE].
static inline const Equity *
rack_info_table_entry_get_best_leaves(const RackInfoTableEntry *entry) {
  return entry->best_leaves;
}

static inline bool rack_info_table_has_context_caps(const RackInfoTable *rit) {
  if (rit == NULL) {
    return false;
  }
  if ((rit->flags & RIT_FLAG_CONTEXT_CAPS_INLINE) != 0) {
    return true;
  }
  return (rit->flags & RIT_FLAG_CONTEXT_CAPS) != 0 &&
         rit->context_capped_best_leaves != NULL &&
         rit->context_capped_nonplaythrough_best_leave_values != NULL;
}

static inline bool rack_info_table_base_matches(const RackInfoTable *rit,
                                                uint64_t base_klv_fingerprint) {
  return rit != NULL && rit->version >= 13 && base_klv_fingerprint != 0 &&
         rit->base_klv_fingerprint == base_klv_fingerprint;
}

static inline bool
rack_info_table_context_matches(const RackInfoTable *rit,
                                uint64_t context_klv_fingerprint) {
  return rack_info_table_has_context_caps(rit) &&
         context_klv_fingerprint != 0 &&
         rit->context_klv_fingerprint == context_klv_fingerprint;
}

static inline size_t
rack_info_table_entry_index(const RackInfoTable *rit,
                            const RackInfoTableEntry *entry) {
  return (size_t)(entry - rit->entries);
}

static inline const Equity *rack_info_table_get_context_capped_best_leaves(
    const RackInfoTable *rit, const RackInfoTableEntry *entry) {
  if (!rack_info_table_has_context_caps(rit)) {
    return NULL;
  }
  if ((rit->flags & RIT_FLAG_CONTEXT_CAPS_INLINE) != 0) {
    return entry->best_leaves;
  }
  return rit->context_capped_best_leaves +
         rack_info_table_entry_index(rit, entry) *
             RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY;
}

static inline const Equity *
rack_info_table_get_context_capped_nonplaythrough_best_leaves(
    const RackInfoTable *rit, const RackInfoTableEntry *entry) {
  if (!rack_info_table_has_context_caps(rit)) {
    return NULL;
  }
  if ((rit->flags & RIT_FLAG_CONTEXT_CAPS_INLINE) != 0) {
    return entry->nonplaythrough_best_leave_values;
  }
  return rit->context_capped_nonplaythrough_best_leave_values +
         rack_info_table_entry_index(rit, entry) *
             RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY;
}

// Returns the inline bingo words and count. Returns 0 if not available
// (>RIT_MAX_INLINE_BINGO_WORDS anagrams, or no bingo exists).
static inline int
rack_info_table_entry_get_bingo_words(const RackInfoTableEntry *entry,
                                      const MachineLetter **words_out) {
  *words_out = entry->bingo_words[0];
  return entry->num_bingo_words;
}

static inline int
rack_info_table_word_entry_get_bingo_words(const RackInfoTableWordEntry *entry,
                                           const MachineLetter **words_out) {
  *words_out = entry->bingo_words[0];
  return entry->num_bingo_words;
}

// Returns the precomputed best exchange strip and its length.
static inline const MachineLetter *
rack_info_table_entry_get_best_exchange_strip(const RackInfoTableEntry *entry,
                                              int *tiles_exchanged) {
  *tiles_exchanged = entry->best_exchange_tiles_exchanged;
  return entry->best_exchange_strip;
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

static inline void rack_info_table_destroy(RackInfoTable *rit) {
  if (!rit) {
    return;
  }
  free(rit->name);
  if (rit->is_mmapped) {
    munmap(rit->mmap_base, rit->mmap_size);
  } else {
    free(rit->bucket_starts);
    free(rit->entries);
    free(rit->word_entries);
    free(rit->context_capped_best_leaves);
    free(rit->context_capped_nonplaythrough_best_leave_values);
  }
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
//   1 byte:  v13 flags (zero padding in v11/v12)
//   4 bytes: num_buckets
//   4 bytes: num_entries
//   (num_buckets + 1) * 4 bytes: bucket_starts
//   num_entries entries, each sizeof(RackInfoTableEntry) bytes:
//     (1 << RACK_SIZE) * 3 bytes: leaves_packed (24-bit signed LE)
//     (RACK_SIZE + 1) * 4 bytes: playthrough_union (uint32_t)
//     (RACK_SIZE + 1) * 4 bytes: nonplaythrough_best_leave_values (Equity)
//     (RACK_SIZE + 1) * 4 bytes: best_leaves (Equity, per-leave-size max)
//     RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS * 4 bytes: multi_pt_tp7_bitvec
//                                                  (uint32 per word length)
//     RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS * 4 bytes: multi_pt_tp6_bitvec
//     1 byte: nonplaythrough_has_word_of_length_bitmask
//     1 byte: num_bingo_words
//     RIT_MAX_INLINE_BINGO_WORDS * RACK_SIZE bytes: bingo_words
//     RACK_SIZE bytes: best_exchange_strip (MachineLetter per tile)
//     1 byte: best_exchange_tiles_exchanged
//     3 bytes: pad
//     16 bytes: bit_rack_bytes (full BitRack for collision check)
//   v13 only, when RIT_FLAG_CONTEXT_CAPS is set (legacy out-of-line layout):
//     num_entries * (RACK_SIZE + 1) * 4 bytes: context capped best leaves
//     num_entries * (RACK_SIZE + 1) * 4 bytes: context capped
//                                                  nonplaythrough best leaves
//   When RIT_FLAG_CONTEXT_CAPS_INLINE is set, those values replace the
//   ordinary best_leaves and nonplaythrough_best_leave_values fields in each
//   entry and no overlay arrays are appended.
//   v13 metadata (always present):
//     8 bytes: "RITMETA\0"
//     8 bytes: base KLV content fingerprint
//     8 bytes: contextual KLV content fingerprint (zero without caps)
//     4 bytes: contextual cap quantile in parts per million
//     4 bytes: reserved
//
// When RIT_FLAG_WORD_ONLY is set, the full entries above are replaced by
// num_entries RackInfoTableWordEntry records. They retain playthrough unions,
// multi-playthrough bitvectors, nonplaythrough existence, inline bingos, and
// the BitRack collision key, but omit every KLV-derived field.

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
    // Packed 24-bit leaves are stored as raw bytes — no endian swap.
    fwrite_or_die(entry->leaves_packed, sizeof(uint8_t),
                  RACK_INFO_TABLE_LEAVES_PACKED_BYTES, stream,
                  "rit leaves packed");
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
    for (int bl_idx = 0; bl_idx < RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY;
         bl_idx++) {
      const uint32_t le_bl = htole32((uint32_t)entry->best_leaves[bl_idx]);
      fwrite_or_die(&le_bl, sizeof(uint32_t), 1, stream, "rit best leave");
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
    fwrite_or_die(&entry->nonplaythrough_has_word_of_length_bitmask,
                  sizeof(uint8_t), 1, stream,
                  "rit nonplaythrough has word of length bitmask");
    fwrite_or_die(&entry->num_bingo_words, sizeof(uint8_t), 1, stream,
                  "rit num bingo words");
    fwrite_or_die(entry->bingo_words, sizeof(MachineLetter),
                  RIT_MAX_INLINE_BINGO_WORDS * RACK_SIZE, stream,
                  "rit bingo words");
    fwrite_or_die(entry->best_exchange_strip, sizeof(MachineLetter), RACK_SIZE,
                  stream, "rit best exchange strip");
    fwrite_or_die(&entry->best_exchange_tiles_exchanged, sizeof(uint8_t), 1,
                  stream, "rit best exchange tiles exchanged");
    fwrite_or_die(entry->pad, sizeof(uint8_t), sizeof(entry->pad), stream,
                  "rit entry pad");
    fwrite_or_die(entry->bit_rack_bytes, sizeof(uint8_t),
                  RACK_INFO_TABLE_BITRACK_BYTES, stream, "rit bit_rack_bytes");
  }
#endif
}

static inline void
rit_write_word_entries_or_die(const RackInfoTableWordEntry *entries, uint32_t n,
                              FILE *stream) {
#if IS_LITTLE_ENDIAN
  fwrite_or_die(entries, sizeof(RackInfoTableWordEntry), n, stream,
                "rit word entries");
#else
  for (uint32_t i = 0; i < n; i++) {
    const RackInfoTableWordEntry *entry = &entries[i];
    for (int union_idx = 0; union_idx < RACK_INFO_TABLE_UNIONS_PER_ENTRY;
         union_idx++) {
      const uint32_t value = htole32(entry->playthrough_union[union_idx]);
      fwrite_or_die(&value, sizeof(value), 1, stream,
                    "rit word playthrough union");
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP7_NUM_WORD_LENGTHS;
         len_idx++) {
      const uint32_t value = htole32(entry->multi_pt_tp7_bitvec[len_idx]);
      fwrite_or_die(&value, sizeof(value), 1, stream,
                    "rit word multi pt tp7 bitvec");
    }
    for (int len_idx = 0; len_idx < RIT_MULTI_PT_TP6_NUM_WORD_LENGTHS;
         len_idx++) {
      const uint32_t value = htole32(entry->multi_pt_tp6_bitvec[len_idx]);
      fwrite_or_die(&value, sizeof(value), 1, stream,
                    "rit word multi pt tp6 bitvec");
    }
    fwrite_or_die(&entry->nonplaythrough_has_word_of_length_bitmask,
                  sizeof(uint8_t), 1, stream,
                  "rit word nonplaythrough existence");
    fwrite_or_die(&entry->num_bingo_words, sizeof(uint8_t), 1, stream,
                  "rit word bingo count");
    fwrite_or_die(entry->bingo_words, sizeof(MachineLetter),
                  RIT_MAX_INLINE_BINGO_WORDS * RACK_SIZE, stream,
                  "rit word bingo words");
    fwrite_or_die(entry->pad, sizeof(uint8_t), sizeof(entry->pad), stream,
                  "rit word entry pad");
    fwrite_or_die(entry->bit_rack_bytes, sizeof(uint8_t),
                  RACK_INFO_TABLE_BITRACK_BYTES, stream,
                  "rit word bit_rack_bytes");
  }
#endif
}

static inline void rit_write_equities_or_die(const Equity *values, size_t n,
                                             FILE *stream,
                                             const char *description) {
#if IS_LITTLE_ENDIAN
  fwrite_or_die(values, sizeof(Equity), n, stream, description);
#else
  for (size_t i = 0; i < n; i++) {
    const uint32_t le = htole32((uint32_t)values[i]);
    fwrite_or_die(&le, sizeof(uint32_t), 1, stream, description);
  }
#endif
}

static inline void rit_write_metadata_or_die(const RackInfoTable *rit,
                                             FILE *stream) {
  static const uint8_t magic[8] = {'R', 'I', 'T', 'M', 'E', 'T', 'A', '\0'};
  fwrite_or_die(magic, sizeof(magic), 1, stream, "rit metadata magic");
  const uint64_t base_fp = htole64(rit->base_klv_fingerprint);
  const uint64_t context_fp = htole64(rit->context_klv_fingerprint);
  fwrite_or_die(&base_fp, sizeof(base_fp), 1, stream,
                "rit base klv fingerprint");
  fwrite_or_die(&context_fp, sizeof(context_fp), 1, stream,
                "rit context klv fingerprint");
  rit_write_uint32_or_die(rit->context_cap_quantile_ppm, stream,
                          "rit context cap quantile");
  rit_write_uint32_or_die(0, stream, "rit metadata reserved");
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
  const uint8_t flags = rit->version >= 13 ? rit->flags : 0;
  fwrite_or_die(&flags, sizeof(flags), 1, stream, "rit header flags");

  rit_write_uint32_or_die(rit->num_buckets, stream, "rit num buckets");
  rit_write_uint32_or_die(rit->num_entries, stream, "rit num entries");
  rit_write_uint32s_or_die(rit->bucket_starts, (size_t)rit->num_buckets + 1,
                           stream, "rit bucket starts");
  if (rack_info_table_is_word_only(rit)) {
    rit_write_word_entries_or_die(rit->word_entries, rit->num_entries, stream);
  } else {
    rit_write_entries_or_die(rit->entries, rit->num_entries, stream);
  }
  if (rit->version >= 13) {
    if ((rit->flags & RIT_FLAG_CONTEXT_CAPS) != 0) {
      const size_t n =
          (size_t)rit->num_entries * RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY;
      rit_write_equities_or_die(rit->context_capped_best_leaves, n, stream,
                                "rit context capped best leaves");
      rit_write_equities_or_die(
          rit->context_capped_nonplaythrough_best_leave_values, n, stream,
          "rit context capped nonplaythrough best leaves");
    }
    rit_write_metadata_or_die(rit, stream);
  }
  fclose_or_die(stream);
}

// Project a full RIT into its model-independent word subset. The output keeps
// identical buckets and entry ordering, so conversion is a linear copy and
// does not need the KLV or WMP that originally built the table.
static inline void
rack_info_table_write_word_only_copy(const RackInfoTable *source,
                                     const char *filename,
                                     ErrorStack *error_stack) {
  if (source == NULL || rack_info_table_is_word_only(source)) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        string_duplicate("word-only RIT conversion requires a full RIT"));
    return;
  }
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const uint8_t version = RIT_VERSION;
  fwrite_or_die(&version, sizeof(version), 1, stream, "rit version");
  fwrite_or_die(&source->rack_size, sizeof(source->rack_size), 1, stream,
                "rit rack size");
  fwrite_or_die(&source->playthrough_min_played_size,
                sizeof(source->playthrough_min_played_size), 1, stream,
                "rit playthrough min played size");
  const uint8_t flags = RIT_FLAG_WORD_ONLY;
  fwrite_or_die(&flags, sizeof(flags), 1, stream, "rit header flags");
  rit_write_uint32_or_die(source->num_buckets, stream, "rit num buckets");
  rit_write_uint32_or_die(source->num_entries, stream, "rit num entries");
  rit_write_uint32s_or_die(source->bucket_starts,
                           (size_t)source->num_buckets + 1, stream,
                           "rit bucket starts");

  enum { WORD_ENTRY_COPY_BATCH = 4096 };
  RackInfoTableWordEntry *batch =
      malloc_or_die(WORD_ENTRY_COPY_BATCH * sizeof(RackInfoTableWordEntry));
  for (uint32_t start = 0; start < source->num_entries;
       start += WORD_ENTRY_COPY_BATCH) {
    const uint32_t remaining = source->num_entries - start;
    const uint32_t count =
        remaining < WORD_ENTRY_COPY_BATCH ? remaining : WORD_ENTRY_COPY_BATCH;
    for (uint32_t i = 0; i < count; i++) {
      rack_info_table_copy_word_entry(&batch[i], &source->entries[start + i]);
    }
    rit_write_word_entries_or_die(batch, count, stream);
  }
  free(batch);
  RackInfoTable metadata = *source;
  metadata.flags = RIT_FLAG_WORD_ONLY;
  metadata.base_klv_fingerprint = 0;
  metadata.context_klv_fingerprint = 0;
  metadata.context_cap_quantile_ppm = 0;
  rit_write_metadata_or_die(&metadata, stream);
  fclose_or_die(stream);
}

// Upgrade an existing layout-compatible RIT by cloning its structural/base
// payload and replacing only its leave-derived maxima with contextual caps.
// This dirties most APFS clone extents, but keeps capped values next to the
// structural entry and avoids the measurable random-access cost of the
// original out-of-line overlay.
static inline void rack_info_table_write_contextual_clone(
    const RackInfoTable *rit, const RackInfoTable *base_rit,
    const char *base_filename, const char *output_filename,
    ErrorStack *error_stack) {
  if (!rack_info_table_has_context_caps(rit) ||
      base_rit->num_buckets != rit->num_buckets ||
      base_rit->num_entries != rit->num_entries) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        string_duplicate("base RIT is incompatible with contextual overlay"));
    return;
  }
  for (uint32_t i = 0; i <= rit->num_buckets; i++) {
    if (base_rit->bucket_starts[i] != rit->bucket_starts[i]) {
      error_stack_push(
          error_stack, ERROR_STATUS_RW_WRITE_ERROR,
          string_duplicate("base RIT bucket layout does not match overlay"));
      return;
    }
  }
  for (uint32_t i = 0; i < rit->num_entries; i++) {
    if (memcmp(base_rit->entries[i].bit_rack_bytes,
               rit->entries[i].bit_rack_bytes,
               RACK_INFO_TABLE_BITRACK_BYTES) != 0 ||
        memcmp(base_rit->entries[i].leaves_packed,
               rit->entries[i].leaves_packed,
               RACK_INFO_TABLE_LEAVES_PACKED_BYTES) != 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_RW_WRITE_ERROR,
          string_duplicate(
              "base RIT rack ordering or KLV2 leaves do not match overlay"));
      return;
    }
  }
  if (access(output_filename, F_OK) == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_WRITABLE,
        get_formatted_string("refusing to overwrite existing RIT: %s",
                             output_filename));
    return;
  }
#if defined(__APPLE__)
  if (clonefile(base_filename, output_filename, 0) != 0) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string("could not clone base RIT %s: %s",
                                          base_filename, strerror(errno)));
    return;
  }
#else
  FILE *source = fopen_safe(base_filename, "rb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  FILE *destination = fopen_safe(output_filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    fclose_or_die(source);
    return;
  }
  uint8_t *buffer = malloc_or_die(1024 * 1024);
  size_t n;
  while ((n = fread(buffer, 1, 1024 * 1024, source)) > 0) {
    fwrite_or_die(buffer, 1, n, destination, "base rit copy");
  }
  free(buffer);
  fclose_or_die(source);
  fclose_or_die(destination);
#endif
  FILE *stream = fopen_safe(output_filename, "r+b", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const size_t entries_offset =
      12 + ((size_t)rit->num_buckets + 1) * sizeof(uint32_t);
  const size_t entries_end =
      entries_offset + (size_t)rit->num_entries * sizeof(RackInfoTableEntry);
  const int fd = fileno(stream);
  if (ftruncate(fd, (off_t)entries_end) != 0) {
    log_fatal("could not resize contextual RIT clone");
  }
  void *mapped =
      mmap(NULL, entries_end, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    log_fatal("could not mmap contextual RIT clone for writing");
  }
  uint8_t *bytes = (uint8_t *)mapped;
  bytes[0] = RIT_VERSION;
  bytes[3] = rit->flags;
  RackInfoTableEntry *output_entries =
      (RackInfoTableEntry *)(bytes + entries_offset);
  for (uint32_t i = 0; i < rit->num_entries; i++) {
    memcpy(output_entries[i].best_leaves, rit->entries[i].best_leaves,
           sizeof(output_entries[i].best_leaves));
    memcpy(output_entries[i].nonplaythrough_best_leave_values,
           rit->entries[i].nonplaythrough_best_leave_values,
           sizeof(output_entries[i].nonplaythrough_best_leave_values));
  }
  if (msync(mapped, entries_end, MS_SYNC) != 0) {
    log_fatal("could not sync contextual RIT clone");
  }
  munmap(mapped, entries_end);
  if (fseek(stream, 0, SEEK_END) != 0) {
    log_fatal("could not seek to end of contextual RIT clone");
  }
  rit_write_metadata_or_die(rit, stream);
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
    // leaves_packed are raw bytes — no endian swap needed.
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
    for (int bl_idx = 0; bl_idx < RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY;
         bl_idx++) {
      entries[i].best_leaves[bl_idx] =
          (Equity)le32toh((uint32_t)entries[i].best_leaves[bl_idx]);
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
    // bit_rack_bytes are already stored little-endian
    // nonplaythrough_has_word_of_length_bitmask is a single byte, no swap
  }
#endif
}

static inline void rit_read_word_entries_or_die(RackInfoTableWordEntry *entries,
                                                uint32_t n, FILE *stream) {
  if (fread(entries, sizeof(RackInfoTableWordEntry), n, stream) != n) {
    log_fatal("could not read word entries from rit stream");
  }
#if !IS_LITTLE_ENDIAN
  for (uint32_t i = 0; i < n; i++) {
    for (int union_idx = 0; union_idx < RACK_INFO_TABLE_UNIONS_PER_ENTRY;
         union_idx++) {
      entries[i].playthrough_union[union_idx] =
          le32toh(entries[i].playthrough_union[union_idx]);
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
  }
#endif
}

static inline void rit_read_equities_or_die(Equity *values, size_t n,
                                            FILE *stream) {
  if (fread(values, sizeof(Equity), n, stream) != n) {
    log_fatal("could not read equities from rit stream");
  }
#if !IS_LITTLE_ENDIAN
  for (size_t i = 0; i < n; i++) {
    values[i] = (Equity)le32toh((uint32_t)values[i]);
  }
#endif
}

static inline void rit_read_metadata_or_die(RackInfoTable *rit, FILE *stream) {
  static const uint8_t expected_magic[8] = {'R', 'I', 'T', 'M',
                                            'E', 'T', 'A', '\0'};
  uint8_t magic[8];
  if (fread(magic, sizeof(magic), 1, stream) != 1 ||
      memcmp(magic, expected_magic, sizeof(magic)) != 0) {
    log_fatal("invalid or missing rit metadata");
  }
  uint64_t base_fp = 0;
  uint64_t context_fp = 0;
  if (fread(&base_fp, sizeof(base_fp), 1, stream) != 1 ||
      fread(&context_fp, sizeof(context_fp), 1, stream) != 1) {
    log_fatal("could not read rit fingerprints");
  }
  rit->base_klv_fingerprint = le64toh(base_fp);
  rit->context_klv_fingerprint = le64toh(context_fp);
  rit_read_uint32_or_die(&rit->context_cap_quantile_ppm, stream);
  uint32_t reserved;
  rit_read_uint32_or_die(&reserved, stream);
}

static inline void rack_info_table_load_from_stream(RackInfoTable *rit,
                                                    FILE *stream,
                                                    const char *filename,
                                                    ErrorStack *error_stack) {
  uint8_t version;
  if (fread(&version, sizeof(version), 1, stream) != 1) {
    log_fatal("could not read rit version");
  }
  if (version < RIT_EARLIEST_SUPPORTED_VERSION || version > RIT_VERSION) {
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

  uint8_t flags;
  if (fread(&flags, sizeof(flags), 1, stream) != 1) {
    log_fatal("could not read rit header flags");
  }
  rit->flags = version >= 13 ? flags : 0;
  if ((rit->flags & ~RIT_KNOWN_FLAGS) != 0) {
    log_fatal("rit contains unsupported flags");
  }
  if (rack_info_table_is_word_only(rit) &&
      (rit->flags & (RIT_FLAG_CONTEXT_CAPS | RIT_FLAG_CONTEXT_CAPS_INLINE)) !=
          0) {
    log_fatal("word-only rit cannot contain contextual leave caps");
  }

  rit_read_uint32_or_die(&rit->num_buckets, stream);
  rit_read_uint32_or_die(&rit->num_entries, stream);

  rit->bucket_starts = (uint32_t *)malloc_or_die(
      ((size_t)rit->num_buckets + 1) * sizeof(uint32_t));
  rit_read_uint32s_or_die(rit->bucket_starts, (size_t)rit->num_buckets + 1,
                          stream);

  if (rack_info_table_is_word_only(rit)) {
    rit->word_entries = (RackInfoTableWordEntry *)malloc_or_die(
        (size_t)rit->num_entries * sizeof(RackInfoTableWordEntry));
    rit_read_word_entries_or_die(rit->word_entries, rit->num_entries, stream);
  } else {
    rit->entries = (RackInfoTableEntry *)malloc_or_die(
        (size_t)rit->num_entries * sizeof(RackInfoTableEntry));
    rit_read_entries_or_die(rit->entries, rit->num_entries, stream);
  }
  if (version >= 13) {
    if ((rit->flags & RIT_FLAG_CONTEXT_CAPS) != 0) {
      const size_t n =
          (size_t)rit->num_entries * RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY;
      rit->context_capped_best_leaves =
          (Equity *)malloc_or_die(n * sizeof(Equity));
      rit->context_capped_nonplaythrough_best_leave_values =
          (Equity *)malloc_or_die(n * sizeof(Equity));
      rit_read_equities_or_die(rit->context_capped_best_leaves, n, stream);
      rit_read_equities_or_die(
          rit->context_capped_nonplaythrough_best_leave_values, n, stream);
    }
    rit_read_metadata_or_die(rit, stream);
  }
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

// mmap-based load (little-endian only). Points bucket_starts and entries
// directly into the mapped file, eliminating the fread startup cost.
static inline void rack_info_table_load_mmap(RackInfoTable *rit,
                                             const char *name,
                                             const char *filename,
                                             ErrorStack *error_stack) {
#if !IS_LITTLE_ENDIAN
  (void)rit;
  (void)name;
  error_stack_push(
      error_stack, ERROR_STATUS_RW_READ_ERROR,
      get_formatted_string(
          "mmap mode is not supported on big-endian architectures: %s",
          filename));
  return;
#else
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
        get_formatted_string("could not open rit file: %s", filename));
    return;
  }
  struct stat st;
  if (fstat(fd, &st) < 0) {
    close(fd);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("could not stat rit file: %s", filename));
    return;
  }
  size_t file_size = (size_t)st.st_size;
  void *mapped = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (mapped == MAP_FAILED) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("could not mmap rit file: %s", filename));
    return;
  }

  if (file_size < 12) {
    munmap(mapped, file_size);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rit file too small for header: %s", filename));
    return;
  }

  const uint8_t *data = (const uint8_t *)mapped;

  // Parse the 12-byte header.
  uint8_t version = data[0];
  if (version < RIT_EARLIEST_SUPPORTED_VERSION || version > RIT_VERSION) {
    munmap(mapped, file_size);
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_UNSUPPORTED_VERSION,
        get_formatted_string(
            "detected rit version %d but only %d or greater is supported: %s\n",
            version, RIT_EARLIEST_SUPPORTED_VERSION, filename));
    return;
  }
  rit->version = version;

  uint8_t rack_size = data[1];
  if (rack_size != RACK_SIZE) {
    munmap(mapped, file_size);
    error_stack_push(error_stack, ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM,
                     get_formatted_string(
                         "rit rack size %d does not match build rack size %d: "
                         "%s\n",
                         rack_size, RACK_SIZE, filename));
    return;
  }
  rit->rack_size = rack_size;
  rit->playthrough_min_played_size = data[2];
  rit->flags = version >= 13 ? data[3] : 0;
  if ((rit->flags & ~RIT_KNOWN_FLAGS) != 0) {
    munmap(mapped, file_size);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rit contains unsupported flags: %s", filename));
    return;
  }
  if (rack_info_table_is_word_only(rit) &&
      (rit->flags & (RIT_FLAG_CONTEXT_CAPS | RIT_FLAG_CONTEXT_CAPS_INLINE)) !=
          0) {
    munmap(mapped, file_size);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("word-only rit contains leave-cap flags: %s",
                             filename));
    return;
  }

  memcpy(&rit->num_buckets, data + 4, sizeof(uint32_t));
  memcpy(&rit->num_entries, data + 8, sizeof(uint32_t));

  // Bounds-check each section against the remaining mapped region. Use
  // division against the available space so corrupt num_buckets/num_entries
  // values cannot overflow the size_t multiplication.
  const size_t bucket_starts_offset = 12;
  const size_t num_bucket_start_slots = (size_t)rit->num_buckets + 1;
  if (num_bucket_start_slots >
      (file_size - bucket_starts_offset) / sizeof(uint32_t)) {
    munmap(mapped, file_size);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rit file truncated or corrupt: %s", filename));
    return;
  }
  const size_t bucket_starts_size = num_bucket_start_slots * sizeof(uint32_t);
  const size_t entries_offset = bucket_starts_offset + bucket_starts_size;
  const size_t entry_size = rack_info_table_is_word_only(rit)
                                ? sizeof(RackInfoTableWordEntry)
                                : sizeof(RackInfoTableEntry);
  if ((size_t)rit->num_entries > (file_size - entries_offset) / entry_size) {
    munmap(mapped, file_size);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rit file truncated or corrupt: %s", filename));
    return;
  }

  rit->bucket_starts = (uint32_t *)(data + bucket_starts_offset);
  if (rack_info_table_is_word_only(rit)) {
    rit->word_entries = (RackInfoTableWordEntry *)(data + entries_offset);
  } else {
    rit->entries = (RackInfoTableEntry *)(data + entries_offset);
  }

  size_t cursor = entries_offset + (size_t)rit->num_entries * entry_size;
  if (version >= 13) {
    if ((rit->flags & RIT_FLAG_CONTEXT_CAPS) != 0) {
      const size_t values_per_overlay =
          (size_t)rit->num_entries * RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY;
      if (values_per_overlay > (file_size - cursor) / (2 * sizeof(Equity))) {
        munmap(mapped, file_size);
        error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                         get_formatted_string(
                             "rit context overlay is truncated: %s", filename));
        return;
      }
      const size_t overlay_bytes = values_per_overlay * sizeof(Equity);
      rit->context_capped_best_leaves = (Equity *)(data + cursor);
      cursor += overlay_bytes;
      rit->context_capped_nonplaythrough_best_leave_values =
          (Equity *)(data + cursor);
      cursor += overlay_bytes;
    }
    if (file_size - cursor < RIT_METADATA_SIZE) {
      munmap(mapped, file_size);
      error_stack_push(
          error_stack, ERROR_STATUS_RW_READ_ERROR,
          get_formatted_string("rit metadata is truncated: %s", filename));
      return;
    }
    static const uint8_t expected_magic[8] = {'R', 'I', 'T', 'M',
                                              'E', 'T', 'A', '\0'};
    if (memcmp(data + cursor, expected_magic, sizeof(expected_magic)) != 0) {
      munmap(mapped, file_size);
      error_stack_push(
          error_stack, ERROR_STATUS_RW_READ_ERROR,
          get_formatted_string("rit metadata is invalid: %s", filename));
      return;
    }
    uint64_t base_fp;
    uint64_t context_fp;
    uint32_t quantile_ppm;
    memcpy(&base_fp, data + cursor + 8, sizeof(base_fp));
    memcpy(&context_fp, data + cursor + 16, sizeof(context_fp));
    memcpy(&quantile_ppm, data + cursor + 24, sizeof(quantile_ppm));
    rit->base_klv_fingerprint = le64toh(base_fp);
    rit->context_klv_fingerprint = le64toh(context_fp);
    rit->context_cap_quantile_ppm = le32toh(quantile_ppm);
  }

  // Suppress readahead: lookups are hash-indexed (random), so the default
  // 16-page readahead wastes I/O on every access.
  madvise(mapped, file_size, MADV_RANDOM);

  rit->is_mmapped = true;
  rit->mmap_base = mapped;
  rit->mmap_size = file_size;
  rit->name = string_duplicate(name);
#endif
}

static inline RackInfoTable *rack_info_table_create(const char *data_paths,
                                                    const char *rit_name,
                                                    bool use_mmap,
                                                    ErrorStack *error_stack) {
  char *rit_filename = data_filepaths_get_readable_filename(
      data_paths, rit_name, DATA_FILEPATH_TYPE_RACK_INFO_TABLE, error_stack);
  RackInfoTable *rit = NULL;
  if (error_stack_is_empty(error_stack)) {
    rit = (RackInfoTable *)calloc_or_die(1, sizeof(RackInfoTable));
    if (use_mmap) {
      rack_info_table_load_mmap(rit, rit_name, rit_filename, error_stack);
    } else {
      rack_info_table_load(rit, rit_name, rit_filename, error_stack);
    }
  }
  free(rit_filename);
  if (!error_stack_is_empty(error_stack)) {
    rack_info_table_destroy(rit);
    rit = NULL;
  }
  return rit;
}

#endif
