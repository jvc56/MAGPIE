#ifndef WMP_MOVEGEN_H
#define WMP_MOVEGEN_H

#include "../def/bit_rack_defs.h"
#include "../def/kwg_defs.h"
#include "../def/move_defs.h"
#include "../def/wmp_defs.h"
#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/leave_map.h"
#include "../ent/rack_info_table.h"
#include "../ent/wmp.h"
#include "../ent/word_info_table.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_POSSIBLE_PLAYTHROUGH_BLOCKS = ((BOARD_DIM / 2) + 1),
  // Block count zero also occupies a row in the anchor table.
  // An anchor keeps one score base for each of the multipliers 1 to this
  // (see wmp_move_gen_update_anchor_score_bases).
  WMP_SCORE_BASE_COUNT = 2,
  // A subrack's score is bounded by the anchor's score bound and by each
  // score base (see wmp_move_gen_subrack_leave_kept).
  WMP_SCORE_BOUND_COUNT = WMP_SCORE_BASE_COUNT + 1,
  // The leave check of wmp_move_gen_get_leave_kept_subracks works on this
  // many subracks at a time. It is the number of bytes in a 64-bit word, not
  // RACK_SIZE + 1: the check writes one result byte per subrack and gathers a
  // word of them into mask bits with one multiplication.
  WMP_LEAVE_KEPT_GROUP_SIZE = 8,
  MAX_WMP_MOVE_GEN_ANCHORS =
      ((RACK_SIZE + 1) * (MAX_POSSIBLE_PLAYTHROUGH_BLOCKS + 1)),
  WMP_ANCHOR_MASK_WORDS = (MAX_WMP_MOVE_GEN_ANCHORS + 63) / 64,
  // Subracks covered by one candidate mask. A RACK_SIZE of 8 has up to 70
  // subracks of one size, so callers take them in chunks of this many.
  WMP_MOVE_GEN_SUBRACKS_PER_MASK = 64,
  WMP_BLANK_WORD_CACHE_BYTES = 16 * 1024,
};

// Nonplaythrough subracks have stable indices for one position. Keep their
// expanded blank words until the next position instead of copying them into
// scratch for every anchor. An offset of zero means absent; stored offsets
// are one-based so the first expansion can start at byte zero.
typedef struct WMPBlankWordCache {
  uint16_t offsets[1 << RACK_SIZE];
  uint16_t word_counts[1 << RACK_SIZE];
  int used_bytes;
  MachineLetter words[WMP_BLANK_WORD_CACHE_BYTES];
} WMPBlankWordCache;

static_assert(WMP_BLANK_WORD_CACHE_BYTES <= UINT16_MAX,
              "Blank word cache offsets must fit in uint16_t");
static_assert(WMP_BLANK_WORD_CACHE_BYTES >=
                  WMP_RESULT_BUFFER_SIZE + WMP_WORD_BUFFER_SLACK_BYTES,
              "Blank word cache must hold a complete expansion with slack");

static inline void wmp_blank_word_cache_reset(WMPBlankWordCache *cache) {
  memset(cache->offsets, 0, sizeof(cache->offsets));
  cache->used_bytes = 0;
}

typedef struct SubrackInfo {
  BitRack subrack;
  const WMPEntry *wmp_entry;
} SubrackInfo;

// A nonplaythrough subrack's wmp_entry has three states: unresolved (this
// sentinel), a resolved miss (NULL), or a resolved entry. Only the RIT-backed
// path ever writes the sentinel, so the non-RIT path is unchanged by lazy
// resolution. The object lives in move_gen.c; only its address matters.
extern const WMPEntry wmp_entry_unresolved_sentinel;
#define WMP_ENTRY_UNRESOLVED (&wmp_entry_unresolved_sentinel)

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
typedef struct WMPMoveGen {
  const WMP *wmp;
  BitRack player_bit_rack;
  int full_rack_size;

  // Tiles already played on board
  BitRack playthrough_bit_rack;
  int num_tiles_played_through;
  // Copies of above to reset after finishing shadow_play_right
  BitRack playthrough_bit_rack_copy;
  int num_tiles_played_through_copy;

  SubrackInfo nonplaythrough_infos[1 << RACK_SIZE];
  // The nonplaythrough subracks' leave values and the sums of their tile
  // scores, with a blank scoring zero, indexed as nonplaythrough_infos is.
  // They are apart from it and side by side for the leave check of
  // wmp_move_gen_get_leave_kept_subracks, which reads them in whole groups
  // and so past the last subrack.
  Equity
      nonplaythrough_leave_values[(1 << RACK_SIZE) + WMP_LEAVE_KEPT_GROUP_SIZE];
  Equity nonplaythrough_tile_score_sums[(1 << RACK_SIZE) +
                                        WMP_LEAVE_KEPT_GROUP_SIZE];
  SubrackInfo playthrough_infos[1 << RACK_SIZE];
  Equity nonplaythrough_best_leave_values[RACK_SIZE + 1];
  bool nonplaythrough_has_word_of_length[RACK_SIZE + 1];
  uint8_t count_by_size[RACK_SIZE + 1];

  Anchor anchors[MAX_WMP_MOVE_GEN_ANCHORS];
  // Bit i marks anchor slot i as touched since the last reset. Typical
  // shadow_play_for_anchor calls touch <=8 of 64 slots. Walking set bits keeps
  // both reset and emission sparse while preserving the full scan's ascending
  // slot order (and therefore deterministic tie order).
  uint64_t touched_anchor_masks[WMP_ANCHOR_MASK_WORDS];
  // Each anchor slot's score bases, and those of the anchors emitted to the
  // heap, indexed by the anchor's insertion_idx. See
  // wmp_move_gen_update_anchor_score_bases.
  Equity anchor_score_bases[MAX_WMP_MOVE_GEN_ANCHORS][WMP_SCORE_BASE_COUNT];
  Equity heap_score_bases[ANCHOR_HEAP_CAPACITY][WMP_SCORE_BASE_COUNT];
  // The scores of the rack's tiles, for the subracks' tile score sums.
  const LetterDistribution *ld;
  int playthrough_blocks;
  int playthrough_blocks_copy;

  // Room for the longest result and the writers' slack.
  MachineLetter buffer[WMP_RESULT_BUFFER_SIZE + WMP_WORD_BUFFER_SLACK_BYTES];
  // Points at the current subrack's contiguous word list: directly into the
  // WMP's storage for blankless subracks (zero copy), or into the position
  // cache or scratch buffer when blank designation assembled the words.
  const MachineLetter *words;
  int tiles_to_play;
  int word_length;
  int num_words;
  Equity leave_value;
  // AND, over this anchor's playthrough blocks, of each block's word info table
  // letter set at this anchor's word length (bit ml set iff some word of that
  // length containing the block uses ml). Accumulated for free while scanning
  // the blocks in wmp_move_gen_set_playthrough_bit_rack. Every placed tile must
  // be one of these letters, so move generation can skip the whole anchor when
  // the rack cannot supply enough of them. All-ones when no table is loaded.
  uint32_t playthrough_addable;
  // Absolute row positions occupied by this anchor's playthrough tiles.
  // Collected during the same block scan as playthrough_bit_rack.
  uint32_t playthrough_positions;
  // When the caller has an active per-rack cache, lazily resolved
  // nonplaythrough WMP entries are written through to it for the next
  // occurrence of the rack. The cache stores the same three-state pointers.
  const WMPEntry **nonplaythrough_wmp_entry_cache;
} WMPMoveGen;

static inline int wmp_move_gen_get_lowest_set_bit(uint64_t bits) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bits);
#else
  int bit = 0;
  while ((bits & 1ULL) == 0) {
    bits >>= 1;
    bit++;
  }
  return bit;
#endif
}

static inline void wmp_move_gen_reset_anchor_slot(WMPMoveGen *wmp_move_gen,
                                                  int slot_idx) {
  wmp_move_gen->anchors[slot_idx].highest_possible_equity = EQUITY_MIN_VALUE;
  wmp_move_gen->anchors[slot_idx].highest_possible_score = EQUITY_MIN_VALUE;
  wmp_move_gen->anchors[slot_idx].rightmost_start_col = 0;
  wmp_move_gen->anchors[slot_idx].leftmost_start_col = BOARD_DIM - 1;
  wmp_move_gen->anchors[slot_idx].tiles_to_play = 0;
}

static inline void wmp_move_gen_reset_anchors(WMPMoveGen *wmp_move_gen) {
  for (int word_idx = 0; word_idx < WMP_ANCHOR_MASK_WORDS; word_idx++) {
    uint64_t touched = wmp_move_gen->touched_anchor_masks[word_idx];
    while (touched != 0) {
      wmp_move_gen_reset_anchor_slot(
          wmp_move_gen,
          word_idx * 64 + wmp_move_gen_get_lowest_set_bit(touched));
      touched &= touched - 1;
    }
    wmp_move_gen->touched_anchor_masks[word_idx] = 0;
  }
}

static inline void wmp_move_gen_init(WMPMoveGen *wmp_move_gen,
                                     const LetterDistribution *ld,
                                     const Rack *player_rack, const WMP *wmp) {
  wmp_move_gen->wmp = wmp;
  wmp_move_gen->ld = ld;
  wmp_move_gen->nonplaythrough_wmp_entry_cache = NULL;
  if (wmp == NULL || player_rack == NULL || ld == NULL) {
    return;
  }
  wmp_move_gen->player_bit_rack = bit_rack_create_from_rack(ld, player_rack);
  wmp_move_gen->full_rack_size = rack_get_total_letters(player_rack);
  // This whole module is sized off RACK_SIZE: count_by_size,
  // nonplaythrough_has_word_of_length, nonplaythrough_best_leave_values and
  // subracks_combination_offsets all have RACK_SIZE + 1 entries, and
  // nonplaythrough_infos holds 1 << RACK_SIZE. An over-full rack would index
  // past all of them, so assert the precondition once here rather than
  // rediscovering it at each use.
  assert(wmp_move_gen->full_rack_size <= RACK_SIZE);
  memset(wmp_move_gen->nonplaythrough_has_word_of_length, false,
         sizeof(wmp_move_gen->nonplaythrough_has_word_of_length));
  // One-time full anchor slot initialization. wmp_move_gen_reset_anchors()
  // is a sparse reset keyed on the touched-anchor masks; untouched
  // slots retain their prior default values rather than being reset. The
  // calloc-zero defaults for highest_possible_equity and leftmost_start_col
  // differ from the expected-on-first-touch sentinels (EQUITY_MIN_VALUE and
  // BOARD_DIM-1 respectively), so we must seed them once here before the
  // sparse-reset protocol takes over.
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    wmp_move_gen->anchors[i].highest_possible_equity = EQUITY_MIN_VALUE;
    wmp_move_gen->anchors[i].highest_possible_score = EQUITY_MIN_VALUE;
    wmp_move_gen->anchors[i].rightmost_start_col = 0;
    wmp_move_gen->anchors[i].leftmost_start_col = BOARD_DIM - 1;
    wmp_move_gen->anchors[i].tiles_to_play = 0;
  }
  memset(wmp_move_gen->touched_anchor_masks, 0,
         sizeof(wmp_move_gen->touched_anchor_masks));
}

static inline bool wmp_move_gen_is_active(const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->wmp != NULL;
}

static const uint8_t subracks_combination_offsets[] = {
    BIT_RACK_COMBINATION_OFFSETS};

static inline uint8_t subracks_get_combination_offset(int size) {
  return subracks_combination_offsets[size];
}

// Walk only the occupied rack letters. Rescanning every BitRack alphabet lane
// at each recursion level is unnecessary when there are at most RACK_SIZE
// occupied letters.
static inline void wmp_move_gen_enumerate_compact_nonplaythrough_subracks(
    WMPMoveGen *wmp_move_gen, BitRack *current,
    const MachineLetter *rack_letters, const uint8_t *rack_letter_counts,
    const Equity *rack_letter_scores, int num_rack_letters, int rack_letter_idx,
    int count, Equity tile_score_sum, LeaveMap *leave_map) {
  if (rack_letter_idx >= num_rack_letters) {
    const int insert_index = subracks_get_combination_offset(count) +
                             wmp_move_gen->count_by_size[count];
    SubrackInfo *subrack_info =
        wmp_move_gen->nonplaythrough_infos + insert_index;
    subrack_info->subrack = *current;
    wmp_move_gen->nonplaythrough_leave_values[insert_index] =
        leave_map_get_current_value(leave_map);
    wmp_move_gen->nonplaythrough_tile_score_sums[insert_index] = tile_score_sum;
    wmp_move_gen->count_by_size[count]++;
    return;
  }

  const MachineLetter ml = rack_letters[rack_letter_idx];
  const int max_num_this = rack_letter_counts[rack_letter_idx];
  const Equity letter_score = rack_letter_scores[rack_letter_idx];
  for (int i = 0; i < max_num_this; i++) {
    wmp_move_gen_enumerate_compact_nonplaythrough_subracks(
        wmp_move_gen, current, rack_letters, rack_letter_counts,
        rack_letter_scores, num_rack_letters, rack_letter_idx + 1, count + i,
        tile_score_sum + i * letter_score, leave_map);
    bit_rack_add_letter(current, ml);
    leave_map_complement_add_letter(leave_map, ml, i);
  }
  wmp_move_gen_enumerate_compact_nonplaythrough_subracks(
      wmp_move_gen, current, rack_letters, rack_letter_counts,
      rack_letter_scores, num_rack_letters, rack_letter_idx + 1,
      count + max_num_this, tile_score_sum + max_num_this * letter_score,
      leave_map);
  for (int i = max_num_this - 1; i >= 0; i--) {
    bit_rack_take_letter(current, ml);
    leave_map_complement_take_letter(leave_map, ml, i);
  }
}

static inline void
wmp_move_gen_enumerate_nonplaythrough_subracks(WMPMoveGen *wmp_move_gen,
                                               LeaveMap *leave_map) {
  MachineLetter rack_letters[RACK_SIZE] = {0};
  uint8_t rack_letter_counts[RACK_SIZE] = {0};
  Equity rack_letter_scores[RACK_SIZE] = {0};
  int num_rack_letters = 0;
  for (int ml = BLANK_MACHINE_LETTER; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    const int count = bit_rack_get_letter(&wmp_move_gen->player_bit_rack, ml);
    if (count > 0) {
      assert(num_rack_letters < RACK_SIZE);
      rack_letters[num_rack_letters] = (MachineLetter)ml;
      rack_letter_counts[num_rack_letters] = (uint8_t)count;
      rack_letter_scores[num_rack_letters] =
          ld_get_score(wmp_move_gen->ld, (MachineLetter)ml);
      num_rack_letters++;
    }
  }
  BitRack empty = bit_rack_create_empty();
  wmp_move_gen_enumerate_compact_nonplaythrough_subracks(
      wmp_move_gen, &empty, rack_letters, rack_letter_counts,
      rack_letter_scores, num_rack_letters, 0, 0, 0, leave_map);
}

static inline void
wmp_move_gen_playthrough_metadata_init(WMPMoveGen *wmp_move_gen,
                                       const Anchor *anchor) {
  wmp_move_gen->word_length = anchor->word_length;
  wmp_move_gen->num_tiles_played_through =
      anchor->word_length - anchor->tiles_to_play;
  wmp_move_gen->tiles_to_play = anchor->tiles_to_play;
}

// Preserve the sequential preparation loop for anchors that survive the
// whole-anchor mask and rack-capacity checks.
static inline void
wmp_move_gen_build_playthrough_subracks(WMPMoveGen *wmp_move_gen) {
  const int subrack_size = wmp_move_gen->tiles_to_play;
  if (wmp_move_gen->num_tiles_played_through == 0) {
    // We can use nonplaythrough subracks
    return;
  }
  const int offset = subracks_get_combination_offset(subrack_size);
  const int count = wmp_move_gen->count_by_size[subrack_size];
  for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
    const SubrackInfo *nonplaythrough_subrack_info =
        &wmp_move_gen->nonplaythrough_infos[offset + idx_for_size];
    SubrackInfo *playthrough_subrack_info =
        &wmp_move_gen->playthrough_infos[offset + idx_for_size];
    playthrough_subrack_info->subrack = nonplaythrough_subrack_info->subrack;
    bit_rack_add_bit_rack(&playthrough_subrack_info->subrack,
                          &wmp_move_gen->playthrough_bit_rack);
  }
}

static inline void
wmp_move_gen_playthrough_subracks_init(WMPMoveGen *wmp_move_gen,
                                       const Anchor *anchor) {
  wmp_move_gen_playthrough_metadata_init(wmp_move_gen, anchor);
  wmp_move_gen_build_playthrough_subracks(wmp_move_gen);
}

// Necessary-only prune for a multi-playthrough case. Tests each
// playthrough letter L against `length_bitvec` (a uint32 with bit L set
// iff some N-tile subrack of this rack + {L} + (other letters) forms a
// word of the target word length, where N is whichever tp the caller
// loaded the bitvec for). If any playthrough letter's bit is clear there
// can be no valid word, so the anchor is pruned.
//
// Returns true if the bitvec proves no word exists (caller should prune).
// Returns false if the bitvec can't prove it.
//
// Iteration walks only the letters actually set in playthrough_bit_rack
// via __builtin_ctzll on each 64-bit half. After the caller-supplied
// length_bitvec load each per-letter test is a register-only
// `(length_bitvec >> ml) & 1` -- no further memory traffic, no per-letter
// byte loads, and the dependency chain is short enough that out-of-order
// execution can overlap the per-letter checks with whatever else
// shadow_record is doing.
static inline bool
wmp_move_gen_multi_pt_bitvec_says_prune(const WMPMoveGen *wmp_move_gen,
                                        uint32_t length_bitvec) {
  const BitRack *pt_br = &wmp_move_gen->playthrough_bit_rack;
  uint64_t low = bit_rack_get_low_64(pt_br);
  while (low != 0) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
    const int bit_idx = __builtin_ctzll(low);
#else
    int bit_idx = 0;
    uint64_t scan = low;
    while ((scan & 1U) == 0) {
      scan >>= 1;
      bit_idx++;
    }
#endif
    const int ml = bit_idx / BIT_RACK_BITS_PER_LETTER;
    if (((length_bitvec >> ml) & 1U) == 0) {
      return true;
    }
    // Clear the entire 4-bit slot for this letter so the next ctz steps
    // to the next set letter.
    low &= ~((uint64_t)0xFU << (ml * BIT_RACK_BITS_PER_LETTER));
  }
  uint64_t high = bit_rack_get_high_64(pt_br);
  while (high != 0) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
    const int bit_idx = __builtin_ctzll(high);
#else
    int bit_idx = 0;
    uint64_t scan = high;
    while ((scan & 1U) == 0) {
      scan >>= 1;
      bit_idx++;
    }
#endif
    const int ml_offset = bit_idx / BIT_RACK_BITS_PER_LETTER;
    const int ml = 16 + ml_offset;
    if (((length_bitvec >> ml) & 1U) == 0) {
      return true;
    }
    high &= ~((uint64_t)0xFU << (ml_offset * BIT_RACK_BITS_PER_LETTER));
  }
  return false;
}

// Returns true and writes the per-(tp, length) bitvec slot to *out_bitvec
// if the (tiles_played, word_length) pair has a populated bitvec slot.
// Returns false otherwise (caller should treat that as "no shadow check
// here" rather than "all letters fail"). Wraps the per-tp
// `rack_info_table_entry_get_multi_pt_tpN_bitvec` accessors so
// shadow_record doesn't have to switch on tiles_played itself, and
// distinguishes the out-of-range case from the genuine all-zeros case
// (which IS a valid prune signal -- "no letter is compatible with this
// rack at this length").
static inline bool wmp_move_gen_get_multi_pt_bitvec_for_tiles_played(
    const RackInfoTableEntry *rit_entry, int tiles_played, int word_length,
    uint32_t *out_bitvec) {
  switch (tiles_played) {
  case RACK_SIZE:
    if (word_length < RIT_MULTI_PT_TP7_MIN_WORD_LENGTH ||
        word_length > RIT_MULTI_PT_TP7_MAX_WORD_LENGTH) {
      return false;
    }
    *out_bitvec =
        rit_entry->multi_pt_tp7_bitvec[word_length -
                                       RIT_MULTI_PT_TP7_MIN_WORD_LENGTH];
    return true;
  case RACK_SIZE - 1:
    if (word_length < RIT_MULTI_PT_TP6_MIN_WORD_LENGTH ||
        word_length > RIT_MULTI_PT_TP6_MAX_WORD_LENGTH) {
      return false;
    }
    *out_bitvec =
        rit_entry->multi_pt_tp6_bitvec[word_length -
                                       RIT_MULTI_PT_TP6_MIN_WORD_LENGTH];
    return true;
  default:
    return false;
  }
}

static inline bool
wmp_move_gen_check_playthrough_full_rack_existence(WMPMoveGen *wmp_move_gen) {
  // Not necessarily a bingo (RACK_SIZE) as this could be in an endgame
  // position, but only one combination is possible, and the leave is empty.
  const int size = wmp_move_gen->full_rack_size;
  const int idx = subracks_get_combination_offset(size);
  SubrackInfo *playthrough_info = &wmp_move_gen->playthrough_infos[idx];
  playthrough_info->subrack = wmp_move_gen->player_bit_rack;
  bit_rack_add_bit_rack(&playthrough_info->subrack,
                        &wmp_move_gen->playthrough_bit_rack);
  const int word_size = size + wmp_move_gen->num_tiles_played_through;
  playthrough_info->wmp_entry = wmp_get_word_entry(
      wmp_move_gen->wmp, &playthrough_info->subrack, word_size);
  return playthrough_info->wmp_entry != NULL;
}

// When wmp_entries_precomputed is true the caller has already populated
// subrack_info->wmp_entry for each canonical subrack of this size, so the
// wmp_get_word_entry hash lookup is skipped. The per-subrack NULL check,
// has_word_of_length update, and leave-max update still run against the
// cached pointers and cached leave values.
static inline void
wmp_move_gen_check_nonplaythroughs_of_size(WMPMoveGen *wmp_move_gen, int size,
                                           bool check_leaves,
                                           bool wmp_entries_precomputed) {
  const int leave_size = wmp_move_gen->full_rack_size - size;
  wmp_move_gen->nonplaythrough_best_leave_values[leave_size] =
      check_leaves ? EQUITY_MIN_VALUE : 0;
  const int offset = subracks_get_combination_offset(size);
  const int count = wmp_move_gen->count_by_size[size];
  for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
    SubrackInfo *subrack_info =
        &wmp_move_gen->nonplaythrough_infos[offset + idx_for_size];
    if (!wmp_entries_precomputed) {
      subrack_info->wmp_entry =
          wmp_get_word_entry(wmp_move_gen->wmp, &subrack_info->subrack, size);
    } else {
      // Only the RIT-backed path leaves entries unresolved, and a rack's path
      // is fixed by whether it has a RIT entry, so a precomputed entry
      // reaching the eager walk must already be resolved.
      assert(subrack_info->wmp_entry != WMP_ENTRY_UNRESOLVED);
    }
    if (subrack_info->wmp_entry == NULL) {
      continue;
    }
    wmp_move_gen->nonplaythrough_has_word_of_length[size] = true;
    if (!check_leaves) {
      continue;
    }
    const Equity leave_value =
        wmp_move_gen->nonplaythrough_leave_values[offset + idx_for_size];
    if (leave_value >
        wmp_move_gen->nonplaythrough_best_leave_values[leave_size]) {
      wmp_move_gen->nonplaythrough_best_leave_values[leave_size] = leave_value;
    }
  }
}

// If subracks_precomputed is true the caller has already populated
// nonplaythrough_infos and count_by_size (e.g. from a per-thread rack
// cache), so we skip the enumerate_nonplaythrough_subracks step.
// If wmp_entries_precomputed is true the caller has also restored
// subrack_info->wmp_entry values, so the per-subrack wmp_get_word_entry
// hash lookup is skipped inside the size walk.
static inline void wmp_move_gen_check_nonplaythrough_existence(
    WMPMoveGen *wmp_move_gen, bool check_leaves, LeaveMap *leave_map,
    bool subracks_precomputed, bool wmp_entries_precomputed) {
  leave_map_set_current_index(leave_map,
                              (1 << wmp_move_gen->full_rack_size) - 1);
  if (!subracks_precomputed) {
    memset(wmp_move_gen->count_by_size, 0, sizeof(wmp_move_gen->count_by_size));
    wmp_move_gen_enumerate_nonplaythrough_subracks(wmp_move_gen, leave_map);
  }
  for (int size = MINIMUM_WORD_LENGTH; size <= wmp_move_gen->full_rack_size;
       size++) {
    wmp_move_gen_check_nonplaythroughs_of_size(wmp_move_gen, size, check_leaves,
                                               wmp_entries_precomputed);
  }
}

// RIT-backed variant of wmp_move_gen_check_nonplaythrough_existence. The RIT
// supplies both the per-size existence result and best leave, so no WMP hash
// walk is needed here. Nonplaythrough WMP entries are resolved lazily if
// record-time generation actually reaches their subrack.
//
// Precondition: rit_entry is non-NULL and corresponds to this move_gen's
// full player_rack.
// If subracks_precomputed is true the caller has already populated
// nonplaythrough_infos and count_by_size (e.g. from a per-thread rack
// cache), so we skip the enumerate_nonplaythrough_subracks step.
static inline void wmp_move_gen_check_nonplaythrough_existence_with_rit(
    WMPMoveGen *wmp_move_gen, bool check_leaves, LeaveMap *leave_map,
    const RackInfoTableEntry *rit_entry, bool subracks_precomputed) {
  leave_map_set_current_index(leave_map,
                              (1 << wmp_move_gen->full_rack_size) - 1);
  if (!subracks_precomputed) {
    memset(wmp_move_gen->count_by_size, 0, sizeof(wmp_move_gen->count_by_size));
    wmp_move_gen_enumerate_nonplaythrough_subracks(wmp_move_gen, leave_map);
  }

  // Seed has_word_of_length and best_leave_values from the RIT entry. The
  // RIT's has_word_of_length_bitmask is an OR over all canonical size-k
  // subracks of this rack, so it matches exactly what
  // wmp_move_gen_check_nonplaythroughs_of_size would compute. Likewise the
  // RIT's best_leave_values[leave_size] is the max leave value over those
  // subracks.
  for (int size = 0; size <= RACK_SIZE; size++) {
    wmp_move_gen->nonplaythrough_has_word_of_length[size] =
        rack_info_table_entry_has_nonplaythrough_word_of_length(rit_entry,
                                                                size);
  }
  for (int leave_size = 0; leave_size <= RACK_SIZE; leave_size++) {
    Equity rit_best = rack_info_table_entry_get_nonplaythrough_best_leave_value(
        rit_entry, leave_size);
    // The non-RIT path writes 0 instead of EQUITY_MIN_VALUE when
    // check_leaves is false. Match that convention so the field is always
    // well-defined downstream.
    if (rit_best == EQUITY_INITIAL_VALUE || !check_leaves) {
      wmp_move_gen->nonplaythrough_best_leave_values[leave_size] =
          check_leaves ? EQUITY_MIN_VALUE : 0;
    } else {
      wmp_move_gen->nonplaythrough_best_leave_values[leave_size] = rit_best;
    }
  }

  // Freshly enumerated subracks carry stale wmp_entry pointers from whatever
  // rack was generated before. Mark the ones record time can reach as
  // unresolved; wmp_move_gen_get_subrack_words resolves them on demand. Sizes
  // with no word are never read, so their pointers are left alone. On a cache
  // hit the restored pointers already carry each slot's state.
  if (!subracks_precomputed) {
    for (int size = MINIMUM_WORD_LENGTH; size <= wmp_move_gen->full_rack_size;
         size++) {
      if (!wmp_move_gen->nonplaythrough_has_word_of_length[size]) {
        continue;
      }
      const int offset = subracks_get_combination_offset(size);
      const int count = wmp_move_gen->count_by_size[size];
      for (int idx = 0; idx < count; idx++) {
        wmp_move_gen->nonplaythrough_infos[offset + idx].wmp_entry =
            WMP_ENTRY_UNRESOLVED;
      }
    }
  }
}

static inline bool wmp_move_gen_nonplaythrough_word_of_length_exists(
    const WMPMoveGen *wmp_move_gen, int word_length) {
  return wmp_move_gen->nonplaythrough_has_word_of_length[word_length];
}

static inline const Equity *wmp_move_gen_get_nonplaythrough_best_leave_values(
    const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->nonplaythrough_best_leave_values;
}

static inline void wmp_move_gen_add_playthrough_letter(WMPMoveGen *wmp_move_gen,
                                                       MachineLetter ml) {
  bit_rack_add_letter(&wmp_move_gen->playthrough_bit_rack, ml);
  wmp_move_gen->num_tiles_played_through++;
}

static inline void
wmp_move_gen_increment_playthrough_blocks(WMPMoveGen *wmp_move_gen) {
  assert(wmp_move_gen->playthrough_blocks < MAX_POSSIBLE_PLAYTHROUGH_BLOCKS);
  wmp_move_gen->playthrough_blocks++;
}

static inline void
wmp_move_gen_save_playthrough_state(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack_copy = wmp_move_gen->playthrough_bit_rack;
  wmp_move_gen->num_tiles_played_through_copy =
      wmp_move_gen->num_tiles_played_through;
  wmp_move_gen->playthrough_blocks_copy = wmp_move_gen->playthrough_blocks;
}

static inline void
wmp_move_gen_restore_playthrough_state(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack = wmp_move_gen->playthrough_bit_rack_copy;
  wmp_move_gen->num_tiles_played_through =
      wmp_move_gen->num_tiles_played_through_copy;
  wmp_move_gen->playthrough_blocks = wmp_move_gen->playthrough_blocks_copy;
}

static inline bool
wmp_move_gen_has_playthrough(const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->num_tiles_played_through > 0;
}

static inline void wmp_move_gen_reset_playthrough(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack = bit_rack_create_empty();
  wmp_move_gen->num_tiles_played_through = 0;
  wmp_move_gen->playthrough_blocks = 0;
}

// Assumes num_tiles_played_through == 1; returns the single machine letter
// that is currently set in playthrough_bit_rack. Uses count-trailing-zeros
// so it's O(1) rather than iterating over the alphabet.
static inline MachineLetter
wmp_move_gen_single_playthrough_letter(const WMPMoveGen *wmp_move_gen) {
  const uint64_t low = bit_rack_get_low_64(&wmp_move_gen->playthrough_bit_rack);
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  // Both halves are indexed and one selected: branching on which half holds
  // the letter mispredicted. The top bit keeps the count defined for an empty
  // half, whose index is then discarded.
  const uint64_t high =
      bit_rack_get_high_64(&wmp_move_gen->playthrough_bit_rack);
  const int low_ml =
      __builtin_ctzll(low | (UINT64_C(1) << 63)) / BIT_RACK_BITS_PER_LETTER;
  const int high_ml = 16 + __builtin_ctzll(high | (UINT64_C(1) << 63)) /
                               BIT_RACK_BITS_PER_LETTER;
  return (MachineLetter)(low != 0 ? low_ml : high_ml);
#else
  if (low != 0) {
    int ml = 0;
    uint64_t v = low;
    while ((v & 0xFU) == 0) {
      v >>= BIT_RACK_BITS_PER_LETTER;
      ml++;
    }
    return (MachineLetter)ml;
  }
  const uint64_t high =
      bit_rack_get_high_64(&wmp_move_gen->playthrough_bit_rack);
  int ml = 16;
  uint64_t v = high;
  while ((v & 0xFU) == 0) {
    v >>= BIT_RACK_BITS_PER_LETTER;
    ml++;
  }
  return (MachineLetter)ml;
#endif
}

static inline int wmp_move_gen_anchor_index(int playthrough_blocks,
                                            int tiles_played) {
  return playthrough_blocks * (RACK_SIZE + 1) + tiles_played;
}

static inline Anchor *wmp_move_gen_get_anchor(WMPMoveGen *wmp_move_gen,
                                              int playthrough_blocks,
                                              int tiles_played) {

  return &wmp_move_gen->anchors[wmp_move_gen_anchor_index(playthrough_blocks,
                                                          tiles_played)];
}

static inline void wmp_move_gen_maybe_update_anchor(WMPMoveGen *wmp_move_gen,
                                                    int tiles_played,
                                                    int word_length,
                                                    int start_col, Equity score,
                                                    Equity equity) {
  assert(start_col >= 0 && start_col < BOARD_DIM);
  const int slot_idx =
      wmp_move_gen_anchor_index(wmp_move_gen->playthrough_blocks, tiles_played);
  Anchor *anchor = &wmp_move_gen->anchors[slot_idx];
  // Marking the slot touched is idempotent, so it is done on every update:
  // testing for the first touch instead mispredicted.
  wmp_move_gen->touched_anchor_masks[slot_idx / 64] |= 1ULL << (slot_idx % 64);
  anchor->tiles_to_play = tiles_played;
  anchor->playthrough_blocks = wmp_move_gen->playthrough_blocks;
  anchor->word_length = word_length;
  // Unconditional stores of the extremes let these compile to selects.
  anchor->leftmost_start_col = start_col < anchor->leftmost_start_col
                                   ? start_col
                                   : anchor->leftmost_start_col;
  anchor->rightmost_start_col = start_col > anchor->rightmost_start_col
                                    ? start_col
                                    : anchor->rightmost_start_col;
  anchor->highest_possible_equity = equity > anchor->highest_possible_equity
                                        ? equity
                                        : anchor->highest_possible_equity;
  anchor->highest_possible_score = score > anchor->highest_possible_score
                                       ? score
                                       : anchor->highest_possible_score;
}

// A score base bounds the scores of an anchor's plays by the tiles they play:
// for every play of the anchor,
//
//   score <= score_bases[base_idx] + (base_idx + 1) * (sum of its tile scores)
//
// The shadow's bound for one placement puts the rack's best tiles on its
// squares, each tile's score times its square's effective multiplier. A play
// there with other tiles scores less by at least the difference of the tile
// score sums times the smallest of those multipliers: sorted by score, the
// play's tiles are each no better than the shadow's. So for a multiplier no
// greater than the smallest one,
//
//   score <= bound - multiplier * (shadow's tile score sum)
//                  + multiplier * (play's tile score sum)
//
// and a greater multiplier only adds to the last term. The base for a
// multiplier is the largest value of the first two terms over the anchor's
// placements, with the multiplier capped at each placement's smallest one.
// The tiles the shadow restricted to one square are in every play of the
// placement, and are counted in both sums.
//
// Call this before wmp_move_gen_maybe_update_anchor: a slot whose score bound
// is still the reset value has no placement yet, and its bases, which are
// not reset, are replaced.
static inline void wmp_move_gen_update_anchor_score_bases(
    WMPMoveGen *wmp_move_gen, int tiles_played, Equity score,
    Equity shadow_tile_score_sum, int smallest_multiplier) {
  const int slot_idx =
      wmp_move_gen_anchor_index(wmp_move_gen->playthrough_blocks, tiles_played);
  Equity *score_bases = wmp_move_gen->anchor_score_bases[slot_idx];
  const bool is_first_placement =
      wmp_move_gen->anchors[slot_idx].highest_possible_score ==
      EQUITY_MIN_VALUE;
  for (int base_idx = 0; base_idx < WMP_SCORE_BASE_COUNT; base_idx++) {
    const int multiplier =
        smallest_multiplier < base_idx + 1 ? smallest_multiplier : base_idx + 1;
    const Equity score_base = score - multiplier * shadow_tile_score_sum;
    score_bases[base_idx] =
        is_first_placement || score_base > score_bases[base_idx]
            ? score_base
            : score_bases[base_idx];
  }
}

static inline void wmp_move_gen_set_playthrough_bit_rack(
    WMPMoveGen *wmp_move_gen, const Anchor *anchor, const Square *row_cache,
    const uint32_t *const *wit_row_lane, const uint8_t *wit_len_lane,
    const WordInfoTable *wit) {
  wmp_move_gen_reset_playthrough(wmp_move_gen);
  wmp_move_gen->playthrough_addable = 0xFFFFFFFFu;
  wmp_move_gen->playthrough_positions = 0;
  if (anchor->playthrough_blocks == 0) {
    return;
  }
  bool in_block = false;
  int blocks_found = 0;
  const uint32_t *positional_values = NULL;
  int positional_block_length = 0;
  int positional_block_col = 0;
  for (int col = anchor->rightmost_start_col; col < BOARD_DIM; col++) {
    const MachineLetter ml = row_cache[col].letter;
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      if (in_block) {
        if (blocks_found == anchor->playthrough_blocks) {
          break;
        }
        in_block = false;
      }
      continue;
    }
    const MachineLetter unblanked_ml = get_unblanked_machine_letter(ml);
    wmp_move_gen_add_playthrough_letter(wmp_move_gen, unblanked_ml);
    wmp_move_gen->playthrough_positions |= 1U << col;
    if (!in_block) {
      in_block = true;
      blocks_found++;
      // `col` is this block's leftmost tile; fold in the cached letters
      // outside this fixed block. A NULL row means uncached
      // or no table loaded (treat as permit-all).
      const uint32_t *block_row =
          wit_row_lane != NULL ? wit_row_lane[col] : NULL;
      if (block_row != NULL) {
        const int block_len = wit_len_lane[col];
        const int extension_len = anchor->word_length - block_len;
        // Shadow anchors can begin inside a longer cached board block. That
        // block is not wholly contained in this candidate word, so its WIT
        // row has no entry at this (negative) extension length. Skipping this
        // optional prune is conservative; normal playthrough validation
        // still rejects candidates that do not match the board.
        if (extension_len >= 0) {
          wmp_move_gen->playthrough_addable &= block_row[extension_len];
          if (anchor->playthrough_blocks >= 2 && wit != NULL &&
              block_len >= WPF_MIN_BLOCK_LENGTH &&
              block_len <= WPF_MAX_BLOCK_LENGTH &&
              block_len > positional_block_length &&
              wit->word_plus_floater[block_len] != NULL) {
            // Shared-KWG board lanes can borrow the other player's WIT.
            // Derive a value ID only for rows owned by the passed table;
            // pointer subtraction between different allocations is undefined.
            const uintptr_t row_address = (uintptr_t)block_row;
            const uintptr_t values_address =
                (uintptr_t)wit->tries[block_len].values;
            const size_t row_bytes =
                (size_t)wit_stride_for_len(block_len) * sizeof(uint32_t);
            if (row_address >= values_address) {
              const uintptr_t offset = row_address - values_address;
              if (offset <
                      (size_t)wit->tries[block_len].num_values * row_bytes &&
                  offset % row_bytes == 0) {
                const size_t value_index = (size_t)offset / row_bytes;
                positional_values =
                    wit->word_plus_floater[block_len] +
                    value_index * word_plus_floater_cells_per_key(block_len);
                positional_block_length = block_len;
                positional_block_col = col;
              }
            }
          }
        }
      }
    }
  }
  assert(blocks_found == anchor->playthrough_blocks);
  if (positional_values == NULL || anchor->playthrough_blocks < 2 ||
      wmp_move_gen->playthrough_addable == 0) {
    return;
  }
  const int extension = anchor->word_length - positional_block_length;
  const int first_length_cell = extension * (extension - 1);
  for (int col = anchor->rightmost_start_col; col < BOARD_DIM; col++) {
    if ((wmp_move_gen->playthrough_positions & (1U << col)) == 0 ||
        (col >= positional_block_col &&
         col < positional_block_col + positional_block_length)) {
      continue;
    }
    const MachineLetter floater =
        get_unblanked_machine_letter(row_cache[col].letter);
    if (floater < 1 || floater > WPF_ALPHABET_SIZE) {
      continue;
    }
    const int delta = col - positional_block_col;
    int offset_index;
    if (delta < 0) {
      if (delta < -extension) {
        wmp_move_gen->playthrough_addable = 0;
        return;
      }
      offset_index = delta + extension;
    } else {
      if (delta >= positional_block_length + extension) {
        wmp_move_gen->playthrough_addable = 0;
        return;
      }
      offset_index = extension + delta - positional_block_length;
    }
    const size_t cell =
        (size_t)(first_length_cell + offset_index) * WPF_ALPHABET_SIZE +
        floater - 1;
    wmp_move_gen->playthrough_addable &= positional_values[cell];
    if (wmp_move_gen->playthrough_addable == 0) {
      return;
    }
  }
}

static inline int
wmp_move_gen_get_num_subrack_combinations(const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->count_by_size[wmp_move_gen->tiles_to_play];
}

static inline const BitRack *
wmp_move_gen_get_nonplaythrough_subrack(const WMPMoveGen *wmp_move_gen,
                                        int idx_for_size) {
  const int offset =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play);
  return &wmp_move_gen->nonplaythrough_infos[offset + idx_for_size].subrack;
}

// 1 when a subrack's leave plus its score bound reaches leave_cutoff, else
// 0. score_bounds[0] is the anchor's score bound, and score_bounds[m] for a
// multiplier m >= 1 is the anchor's score base for it (see
// wmp_move_gen_update_anchor_score_bases), which bounds the score with m times
// the subrack's own tile scores. The subrack's bound is the least of them.
static inline __attribute__((always_inline)) uint64_t
wmp_move_gen_subrack_leave_kept(Equity leave_value, Equity tile_score_sum,
                                const int64_t *score_bounds,
                                int64_t leave_cutoff) {
  int64_t subrack_score_bound = score_bounds[0];
  for (int multiplier = 1; multiplier < WMP_SCORE_BOUND_COUNT; multiplier++) {
    const int64_t tile_bound =
        score_bounds[multiplier] + (int64_t)multiplier * tile_score_sum;
    subrack_score_bound =
        tile_bound < subrack_score_bound ? tile_bound : subrack_score_bound;
  }
  const int64_t leave_bound = (int64_t)leave_value + subrack_score_bound;
  return (uint64_t)(leave_bound >= leave_cutoff);
}

// Bit i of the result is wmp_move_gen_subrack_leave_kept for subrack i of the
// count whose leave values and tile score sums are given, for a cutoff an
// equity can reach.
//
// A subrack is kept when its leave plus each of its score bounds reaches the
// cutoff, so each bound gives a threshold for the leave plus the tile scores
// times the bound's multiplier. The thresholds are found once, and the
// subracks are then compared in groups with no dependence between them, which
// the compiler turns into vector compares. Each result is a byte, and a
// multiplication gathers a group's bytes into bits.
static inline __attribute__((always_inline)) uint64_t
wmp_move_gen_get_leave_kept_subracks(const Equity *leave_values,
                                     const Equity *tile_score_sums, int count,
                                     const int64_t *score_bounds,
                                     int64_t leave_cutoff) {
  // A threshold below every Equity keeps every subrack either way. One above
  // every Equity would drop them all and may keep one here, which is safe.
  int32_t thresholds[WMP_SCORE_BOUND_COUNT];
  for (int multiplier = 0; multiplier < WMP_SCORE_BOUND_COUNT; multiplier++) {
    const int64_t threshold = leave_cutoff - score_bounds[multiplier];
    thresholds[multiplier] =
        threshold < INT32_MIN
            ? INT32_MIN
            : (threshold > INT32_MAX ? INT32_MAX : (int32_t)threshold);
  }
  uint8_t kept_bytes[WMP_MOVE_GEN_SUBRACKS_PER_MASK];
  const int padded_count = (count + WMP_LEAVE_KEPT_GROUP_SIZE - 1) &
                           ~(WMP_LEAVE_KEPT_GROUP_SIZE - 1);
  for (int i = 0; i < padded_count; i++) {
    int32_t weighted_leave = leave_values[i];
    uint8_t kept = (uint8_t)(weighted_leave >= thresholds[0]);
    for (int multiplier = 1; multiplier < WMP_SCORE_BOUND_COUNT; multiplier++) {
      weighted_leave += tile_score_sums[i];
      kept &= (uint8_t)(weighted_leave >= thresholds[multiplier]);
    }
    kept_bytes[i] = kept;
  }
  uint64_t leave_kept = 0;
  for (int i = 0; i < padded_count; i += WMP_LEAVE_KEPT_GROUP_SIZE) {
    uint64_t group;
    memcpy(&group, &kept_bytes[i], sizeof(group));
    // Byte k of the group, 0 or 1, lands on bit 56 + k of the product.
    leave_kept |= ((group * 0x0102040810204080ULL) >> 56) << i;
  }
  return count == WMP_MOVE_GEN_SUBRACKS_PER_MASK
             ? leave_kept
             : leave_kept & (((uint64_t)1 << count) - 1);
}

// Bit i of the result marks subrack first_idx + i of the current anchor's size
// as a candidate, one that might record a play. A clear bit means the subrack
// would record nothing:
// - Its lookup would be a blankless miss that the filter rejects. Every
//   playthrough subrack is looked up, and so is a lazy nonplaythrough subrack
//   that is still unresolved. A subrack with a blank is always a candidate.
// - Its nonplaythrough entry is already resolved to NULL.
// - prune_by_leave is set and its leave plus score_bound is below cutoff. The
//   caller sets it only when the subrack's own leave check would skip such a
//   subrack whenever it is reached.
// Bits of *filter_misses mark the subracks that are still unresolved, kept by
// the leave check, and rejected by the filter. Looking one up would store
// NULL, so the caller stores that with wmp_move_gen_resolve_filter_miss. A
// rejected playthrough subrack keeps its old wmp_entry instead of NULL, which
// nothing reads: a playthrough entry is written before every read.
//
// The leave check needs no lookup, so it runs over the subracks first, and
// only those it keeps are hashed and filtered: in a sim, it drops about a third
// of them. On the lazy path, a nonplaythrough subrack an earlier anchor
// resolved is decided by its entry and not filtered either: its filter word
// would only be loaded to be thrown away. Every subrack filtered gets the same
// straight-line work with no branch on what it holds, so the filter loads
// overlap and no branch mispredicts. The eager path looks up every
// nonplaythrough subrack before generation, so for a nonplaythrough anchor
// there the entry alone decides and the filter, which would only repeat it, is
// skipped.
static inline __attribute__((always_inline)) uint64_t
wmp_move_gen_get_candidate_subracks(const WMPMoveGen *wmp_move_gen,
                                    int first_idx, int count, bool lazy,
                                    bool prune_by_leave,
                                    const int64_t *score_bounds, Equity cutoff,
                                    uint64_t *filter_misses) {
  assert(count <= WMP_MOVE_GEN_SUBRACKS_PER_MASK);
  const int offset =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play) + first_idx;
  const bool is_playthrough =
      wmp_move_gen->word_length > wmp_move_gen->tiles_to_play;
  const SubrackInfo *nonplaythrough_infos =
      &wmp_move_gen->nonplaythrough_infos[offset];
  // With no prune every subrack passes, whatever its leave value holds.
  const uint64_t all_subracks = count == WMP_MOVE_GEN_SUBRACKS_PER_MASK
                                    ? ~(uint64_t)0
                                    : ((uint64_t)1 << count) - 1;
  const uint64_t leave_kept =
      prune_by_leave
          ? wmp_move_gen_get_leave_kept_subracks(
                &wmp_move_gen->nonplaythrough_leave_values[offset],
                &wmp_move_gen->nonplaythrough_tile_score_sums[offset], count,
                score_bounds, cutoff)
          : all_subracks;
  uint64_t candidates = 0;
  if (!lazy && !is_playthrough) {
    for (int bit_idx = 0; bit_idx < count; bit_idx++) {
      candidates |= (uint64_t)(nonplaythrough_infos[bit_idx].wmp_entry != NULL)
                    << bit_idx;
    }
    *filter_misses = 0;
    return candidates & leave_kept;
  }
  const SubrackInfo *lookup_infos =
      is_playthrough ? &wmp_move_gen->playthrough_infos[offset]
                     : nonplaythrough_infos;
  const WMPForLength *wfl = &wmp_move_gen->wmp->wfls[wmp_move_gen->word_length];
  // A playthrough anchor's lookup decides alone, so every subrack kept is
  // looked up. On the lazy nonplaythrough path, only the unresolved ones are,
  // and a resolved one is a candidate when its entry is not NULL.
  uint64_t looked_up = leave_kept;
  if (!is_playthrough) {
    looked_up = 0;
    for (uint64_t remaining = leave_kept; remaining != 0;
         remaining &= remaining - 1) {
      const int bit_idx = wmp_move_gen_get_lowest_set_bit(remaining);
      const WMPEntry *entry = nonplaythrough_infos[bit_idx].wmp_entry;
      looked_up |= (uint64_t)(entry == WMP_ENTRY_UNRESOLVED) << bit_idx;
      candidates |= (uint64_t)(entry != NULL && entry != WMP_ENTRY_UNRESOLVED)
                    << bit_idx;
    }
  }
  uint64_t may_have_words_mask = 0;
  for (uint64_t remaining = looked_up; remaining != 0;
       remaining &= remaining - 1) {
    const int bit_idx = wmp_move_gen_get_lowest_set_bit(remaining);
    // Each test is a 0 or 1 combined with bitwise operators, so the filter
    // load is not conditional and nothing branches.
    const BitRack *subrack = &lookup_infos[bit_idx].subrack;
    const uint64_t has_blank =
        (uint64_t)(bit_rack_get_letter(subrack, BLANK_MACHINE_LETTER) != 0);
    may_have_words_mask |=
        (has_blank | (uint64_t)wfl_blankless_rack_may_have_words(wfl, subrack))
        << bit_idx;
  }
  *filter_misses = is_playthrough ? 0 : looked_up & ~may_have_words_mask;
  return candidates | may_have_words_mask;
}

// Stores the NULL that looking up this lazy nonplaythrough subrack would
// store, given that the filter rejected it, in the subrack and in the rack's
// cache.
static inline void wmp_move_gen_resolve_filter_miss(WMPMoveGen *wmp_move_gen,
                                                    int idx_for_size) {
  const int subrack_idx =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play) +
      idx_for_size;
  SubrackInfo *subrack_info = &wmp_move_gen->nonplaythrough_infos[subrack_idx];
  assert(subrack_info->wmp_entry == WMP_ENTRY_UNRESOLVED);
  assert(wmp_get_word_entry(wmp_move_gen->wmp, &subrack_info->subrack,
                            wmp_move_gen->word_length) == NULL);
  subrack_info->wmp_entry = NULL;
  if (wmp_move_gen->nonplaythrough_wmp_entry_cache != NULL) {
    wmp_move_gen->nonplaythrough_wmp_entry_cache[subrack_idx] = NULL;
  }
}

// Called once per candidate subrack from the hottest loop in wordmap_gen.
// clang inlines it there while it has a single caller; a second caller
// anywhere in that translation unit flips the cost model and turns the loop
// into a call per subrack (measured: about -5% sim throughput). Pin it.
static inline __attribute__((always_inline)) bool
wmp_move_gen_get_subrack_words(WMPMoveGen *wmp_move_gen, int idx_for_size,
                               bool lazy, WMPBlankWordCache *blank_cache) {
  const int offset =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play);
  const int subrack_idx = offset + idx_for_size;
  const bool is_playthrough =
      wmp_move_gen->word_length > wmp_move_gen->tiles_to_play;
  SubrackInfo *subrack_info =
      is_playthrough ? &wmp_move_gen->playthrough_infos[subrack_idx]
                     : &wmp_move_gen->nonplaythrough_infos[subrack_idx];
  if (is_playthrough) {
    subrack_info->wmp_entry = wmp_get_word_entry(
        wmp_move_gen->wmp, &subrack_info->subrack, wmp_move_gen->word_length);
  } else if (lazy && subrack_info->wmp_entry == WMP_ENTRY_UNRESOLVED) {
    subrack_info->wmp_entry = wmp_get_word_entry(
        wmp_move_gen->wmp, &subrack_info->subrack, wmp_move_gen->word_length);
    if (wmp_move_gen->nonplaythrough_wmp_entry_cache != NULL) {
      wmp_move_gen->nonplaythrough_wmp_entry_cache[subrack_idx] =
          subrack_info->wmp_entry;
    }
  }

  if (subrack_info->wmp_entry == NULL) {
    return false;
  }

  assert(wmp_move_gen->word_length >= MINIMUM_WORD_LENGTH);
  assert(wmp_move_gen->word_length <= BOARD_DIM);

  if (bit_rack_get_letter(&subrack_info->subrack, BLANK_MACHINE_LETTER) == 0) {
    // Blankless: point directly at the WMP's contiguous word storage
    // instead of copying the words into the buffer.
    wmp_move_gen->words = wmp_entry_get_blankless_words(
        subrack_info->wmp_entry,
        &wmp_move_gen->wmp->wfls[wmp_move_gen->word_length],
        wmp_move_gen->word_length, &wmp_move_gen->num_words);
    assert(wmp_move_gen->num_words > 0);
    return true;
  }

  if (!is_playthrough && blank_cache->offsets[subrack_idx] != 0) {
    wmp_move_gen->words =
        blank_cache->words + blank_cache->offsets[subrack_idx] - 1;
    wmp_move_gen->num_words = blank_cache->word_counts[subrack_idx];
    return true;
  }
  // A playthrough's key also includes board letters, so its canonical rack
  // index alone cannot identify its words. Keep those expansions in scratch.
  // Reserve the maximum write, including inline-copy slack, before choosing
  // the cache buffer. Running out of space never changes the generated words.
  const bool cache_words =
      !is_playthrough &&
      blank_cache->used_bytes <= WMP_BLANK_WORD_CACHE_BYTES -
                                     WMP_RESULT_BUFFER_SIZE -
                                     WMP_WORD_BUFFER_SLACK_BYTES;
  MachineLetter *buffer = cache_words
                              ? blank_cache->words + blank_cache->used_bytes
                              : wmp_move_gen->buffer;
  const int result_bytes = wmp_entry_write_words_to_buffer(
      subrack_info->wmp_entry, wmp_move_gen->wmp, &subrack_info->subrack,
      wmp_move_gen->word_length, buffer);
  assert(result_bytes > 0);
  assert(result_bytes % wmp_move_gen->word_length == 0);
  wmp_move_gen->num_words = result_bytes / wmp_move_gen->word_length;
  wmp_move_gen->words = buffer;
  if (cache_words) {
    blank_cache->offsets[subrack_idx] = (uint16_t)(blank_cache->used_bytes + 1);
    blank_cache->word_counts[subrack_idx] = (uint16_t)wmp_move_gen->num_words;
    blank_cache->used_bytes += result_bytes;
  }
  return true;
}

static inline void wmp_move_gen_add_anchors(WMPMoveGen *wmp_move_gen, int row,
                                            int col, int last_anchor_col,
                                            int dir,
                                            Equity inference_cutoff_equity,
                                            AnchorHeap *anchor_heap) {
  for (int word_idx = 0; word_idx < WMP_ANCHOR_MASK_WORDS; word_idx++) {
    uint64_t touched = wmp_move_gen->touched_anchor_masks[word_idx];
    while (touched != 0) {
      const int slot_idx =
          word_idx * 64 + wmp_move_gen_get_lowest_set_bit(touched);
      const Anchor *anchor = &wmp_move_gen->anchors[slot_idx];

      // Skip subanchors whose highest possible equity is below the cutoff
      // threshold. This is safe when cutoff_equity is fixed (as in inference
      // with stop_on_threshold).
      if (inference_cutoff_equity == EQUITY_MAX_VALUE ||
          anchor->highest_possible_equity >= inference_cutoff_equity) {
        assert(anchor->word_length >= MINIMUM_WORD_LENGTH);
        assert(anchor->word_length <= wmp_move_gen->wmp->board_dim);
        memcpy(wmp_move_gen->heap_score_bases[anchor_heap->count],
               wmp_move_gen->anchor_score_bases[slot_idx],
               sizeof(wmp_move_gen->anchor_score_bases[slot_idx]));
        anchor_heap_add_unheaped_wmp_anchor(
            anchor_heap, row, col, last_anchor_col, anchor->leftmost_start_col,
            anchor->rightmost_start_col, dir, anchor->highest_possible_equity,
            anchor->highest_possible_score, anchor->tiles_to_play,
            anchor->playthrough_blocks, anchor->word_length);
      }
      wmp_move_gen_reset_anchor_slot(wmp_move_gen, slot_idx);
      touched &= touched - 1;
    }
    // Emitting resets every touched slot, so the next anchor's reset finds
    // nothing to walk.
    wmp_move_gen->touched_anchor_masks[word_idx] = 0;
  }
}

static inline const MachineLetter *
wmp_move_gen_get_word(const WMPMoveGen *wmp_move_gen, int word_idx) {
  return wmp_move_gen->words + word_idx * wmp_move_gen->word_length;
}

static inline Equity
wmp_move_gen_get_leave_value(const WMPMoveGen *wmp_move_gen, int subrack_idx) {
  const int offset =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play);
  return wmp_move_gen->nonplaythrough_leave_values[offset + subrack_idx];
}
#endif
