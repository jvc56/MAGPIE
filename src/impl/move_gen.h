#ifndef MOVE_GEN_H
#define MOVE_GEN_H

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/leave_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/rack_info_table.h"
#include "wmp_move_gen.h"
#include <stdbool.h>
#include <stdint.h>

#define MOVEGEN_RIT_CACHE_SIZE 64

// Per-thread cache of KLV walk results (leave_values[128] and
// best_leaves[8]) keyed by player_bit_rack. This is the "mini-RIT":
// the same data a loaded RackInfoTable would have supplied, computed
// on demand on cache miss and reused on recurring racks in the sim
// rollouts. Lets WMP-enabled runs without a .rit file still skip the
// per-rack KLV/KWG descent once the rack has been seen. Direct-mapped.
#ifndef MOVEGEN_KLV_LEAVES_CACHE_SIZE
#define MOVEGEN_KLV_LEAVES_CACHE_SIZE 256
#endif

typedef struct KlvLeavesCacheEntry {
  BitRack key;
  bool valid;
  // Per-subset (128-slot) leave_values, same layout as leave_map.leave_values.
  Equity leave_values[1 << RACK_SIZE];
  // Per-leave-size max leave across canonical subsets, same as
  // RackInfoTableEntry.best_leaves.
  Equity best_leaves[RACK_SIZE + 1];
} KlvLeavesCacheEntry;

// Size of the per-thread cache of
// wmp_move_gen_enumerate_nonplaythrough_subracks results. Keyed by
// player_bit_rack. The enumeration output is a function of the rack alone, so
// caching it saves 5-20% of sim time when the same rack recurs across rollouts
// (typical in MCTS-style sims). Direct-mapped.
#ifndef MOVEGEN_SUBRACK_CACHE_SIZE
#define MOVEGEN_SUBRACK_CACHE_SIZE 64
#endif

// Number of subrack slots stored per rack. Matches (1 << RACK_SIZE) which
// is the total number of multi-subset combinations of a RACK_SIZE-tile rack.
#define MOVEGEN_SUBRACK_CACHE_ENTRIES (1 << RACK_SIZE)

typedef struct SubrackEnumCacheEntry {
  BitRack key;
  bool valid;
  // Flat array indexed by subracks_get_combination_offset(size) + idx_for_size.
  BitRack subracks[MOVEGEN_SUBRACK_CACHE_ENTRIES];
  Equity leave_values[MOVEGEN_SUBRACK_CACHE_ENTRIES];
  // wmp_entry pointers per subrack, also rack-determined. Storing them
  // lets us skip the per-subrack wmp_get_word_entry hash lookups on
  // cache hit. Invalidated when the WMP pointer changes.
  const WMPEntry *wmp_entries[MOVEGEN_SUBRACK_CACHE_ENTRIES];
  uint8_t count_by_size[RACK_SIZE + 1];
} SubrackEnumCacheEntry;

// Per-thread cache of wordmap_gen results keyed on
// (player_rack, anchor descriptor, local row_cache state). Stores an
// upper bound on the equity the anchor can produce for this rack+board
// neighborhood. On hit, if the current best equity is already >= the
// cached bound, the anchor is skipped entirely. Sized as a direct-mapped
// table indexed by low bits of the key hash.
//
// The 128k default was tuned on simbench (normal sim, plies=2, mi=30):
//   size     hit%    skip%    iters/sec change
//   256      1.5%    0.8%    -3.5%
//   1024     4.7%    2.5%    -3.0%
//   4096    11.2%    6.2%    -0.5%
//   16384   17.7%    9.6%     0%
//   65536   24.3%    13.0%   +1.0%
//   131072  28.2%    15.0%   +5.6%  <- current default
//   262144  32.3%    17.2%   +4.4%  (+0.1% more hits, +1.5% more memory)
//   524288  36.3%    19.5%   +4.3%  (L3 pressure starts)
//   1048576 39.4%    21.2%   +0.6%
#ifndef MOVEGEN_ANCHOR_CACHE_SIZE
#define MOVEGEN_ANCHOR_CACHE_SIZE 131072
#endif

// Optional: only cache anchors whose word_length is in [MIN, MAX].
// At MOVEGEN_ANCHOR_CACHE_SIZE=262144 every length pays its hash cost
// back so gating hurts; left configurable for smaller-cache experiments
// or workloads where short-anchor overhead dominates.
#ifndef MOVEGEN_ANCHOR_CACHE_MIN_LENGTH
#define MOVEGEN_ANCHOR_CACHE_MIN_LENGTH 2
#endif
#ifndef MOVEGEN_ANCHOR_CACHE_MAX_LENGTH
#define MOVEGEN_ANCHOR_CACHE_MAX_LENGTH 15
#endif

typedef struct RackAnchorCacheEntry {
  uint64_t key_hash; // 0 if empty
  // Upper bound on the equity this rack+anchor can produce. For fully-
  // searched entries it is the max equity actually observed. For
  // partially-searched entries it is max(max_observed, cutoff_at_exit),
  // which bounds plays from pruned subracks since the prune check fires
  // only when cutoff >= leave + highest_possible_score (an upper bound
  // on the pruned subracks' plays). Either way, a caller whose current
  // best equity meets or exceeds this can skip the anchor.
  Equity upper_bound;
} RackAnchorCacheEntry;

typedef struct UnrestrictedMultiplier {
  uint8_t multiplier;
  uint8_t column;
} UnrestrictedMultiplier;

typedef struct MoveGen {
  // Owned by this MoveGen struct
  int current_row_index;
  int current_anchor_col;
  uint64_t anchor_left_extension_set;
  uint64_t anchor_right_extension_set;

  int last_anchor_col;
  int dir;
  int max_tiles_to_play;
  int tiles_played;
  int first_played_tile_col;
  int number_of_plays;
  int move_sort_type;
  move_record_t move_record_type;
  int number_of_tiles_in_bag;
  int player_index;
  Equity bingo_bonus;
  bool kwgs_are_shared;
  bool is_wordsmog;
  Rack player_rack;
  Rack player_rack_shadow_right_copy;
  // Using to save the player's full rack
  // for shadow playing and then is later
  // used for alpha generation
  Rack full_player_rack;
  Rack bingo_alpha_rack;
  Rack bingo_alpha_rack_shadow_right_copy;
  Rack opponent_rack;
  Rack leave;
  Square lanes_cache[BOARD_DIM * BOARD_DIM * 2];
  Square row_cache[BOARD_DIM];
  uint8_t row_number_of_anchors_cache[(BOARD_DIM) * 2];
  Equity opening_move_penalties[(BOARD_DIM) * 2];
  int board_number_of_tiles_played;
  int cross_index;
  Move best_move_and_current_move[2];
  int best_move_index;
  Equity current_anchor_highest_possible_score;
  // Updated every time a play is recorded
  Equity cutoff_equity_or_score;
  // This field is only used for the MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST
  // record type
  Equity best_move_equity_or_score;
  Equity eq_margin_movegen;

  // Inference cutoff fields
  Equity target_equity_cutoff;
  int target_leave_size;
  bool stop_on_threshold;
  bool threshold_exceeded;

  MachineLetter strip[(MOVE_MAX_TILES)];
  MachineLetter exchange_strip[(MOVE_MAX_TILES)];
  // MOVE_RECORD_TILES_PLAYED mode fields:
  // Bitvector of machine letters that appear in any valid move.
  // Bit i is set if machine letter i is playable.
  uint64_t tiles_played_bv;
  // Bitvector of machine letters on the rack (target for early exit).
  // Generation stops when (tiles_played_bv & target_tiles_bv) ==
  // target_tiles_bv.
  uint64_t target_tiles_bv;
  LeaveMap leave_map;
  BitRack player_bit_rack;
  // Shadow plays
  int current_left_col;
  int current_right_col;

  // Used to insert "unrestricted" multipliers into a descending list for
  // calculating the maximum score for an anchor. We don't know which tiles will
  // go in which multipliers so we keep a sorted list. The inner product of
  // those and the descending tile scores is the highest possible score of a
  // permutation of tiles in those squares.
  UnrestrictedMultiplier
      descending_cross_word_multipliers[WORD_ALIGNING_RACK_SIZE];
  uint16_t descending_effective_letter_multipliers[WORD_ALIGNING_RACK_SIZE];
  uint8_t num_unrestricted_multipliers;
  uint8_t last_word_multiplier;

  // Used to reset the arrays after finishing shadow_play_right, which may have
  // rearranged the ordering of the multipliers used while shadowing left.
  UnrestrictedMultiplier desc_xw_muls_copy[WORD_ALIGNING_RACK_SIZE];
  uint16_t desc_eff_letter_muls_copy[WORD_ALIGNING_RACK_SIZE];

  // Since shadow does not have backtracking besides when switching from going
  // right back to going left, it is convenient to store these parameters here
  // rather than using function arguments for them.

  // This is a sum of already-played crosswords and tiles restricted to a known
  // empty square (times a letter or word multiplier). It's a part of the score
  // not affected by the overall mainword multiplier.
  Equity shadow_perpendicular_additional_score;

  // This is a sum of both the playthrough tiles and tiles restricted to a known
  // empty square (times their letter multiplier). It will be multiplied by
  // shadow_word_multiplier as part of computing score shadow_record.
  Equity shadow_mainword_restricted_score;

  // Product of word multipliers used by newly played tiles.
  int shadow_word_multiplier;

  Equity highest_shadow_equity;
  Equity highest_shadow_score;
  int number_of_letters_on_rack;
  const KWG *kwg;
  const KLV *klv;
  // Snapshot of klv->mutation_counter captured at the last gen_load_position
  // call. If the KLV's leave_values have been mutated in place since then
  // (test-only set_klv_leave_value path), leave-derived caches (subrack
  // cache, anchor cache upper bounds) must be invalidated even though the
  // KLV pointer is unchanged.
  uint64_t klv_mutation_counter_at_load;
  const RackInfoTable *rack_info_table;
  // RIT entry for the current player_rack, looked up once in
  // gen_look_up_leaves_and_record_exchanges and cached here for the duration
  // of this move generation. NULL if rack_info_table is NULL, the rack isn't
  // a full RACK_SIZE rack, or the rack wasn't found in the table.
  const RackInfoTableEntry *rit_entry;
  // Small per-thread RIT lookup cache. In sim rollouts, the same racks
  // recur across iterations within a turn (limited bag composition).
  // Direct-mapped by low bits of BitRack hash.
  BitRack rit_cache_keys[MOVEGEN_RIT_CACHE_SIZE];
  const RackInfoTableEntry *rit_cache_entries[MOVEGEN_RIT_CACHE_SIZE];
  bool rit_cache_valid[MOVEGEN_RIT_CACHE_SIZE];
  // Cache of wmp_move_gen_enumerate_nonplaythrough_subracks output
  // (purely rack-determined). Hit rate tracks rack-repeat rate in sims.
  SubrackEnumCacheEntry subrack_cache[MOVEGEN_SUBRACK_CACHE_SIZE];
  // Per-thread mini-RIT: cached leave_values and best_leaves per full rack.
  // Populated from the KLV walk (generate_exchange_moves) on first touch
  // for each rack; reused on subsequent movegen calls when the same rack
  // recurs. Lets WMP-enabled runs without a loaded RackInfoTable still
  // amortize the KLV descent cost across the rollout. Invalidated when
  // the KLV pointer or its mutation_counter changes.
  KlvLeavesCacheEntry klv_leaves_cache[MOVEGEN_KLV_LEAVES_CACHE_SIZE];
#ifdef ANCHOR_CACHE_ENABLE
  // Anchor-level pruning cache. Updated per wordmap_gen call and checked
  // at the top to skip anchors whose best_equity bound is already
  // dominated by gen->best_move_equity_or_score.
  RackAnchorCacheEntry anchor_cache[MOVEGEN_ANCHOR_CACHE_SIZE];
#endif
  // Scratch slot: the max equity recorded by the currently-executing
  // wordmap_gen call, observed via
  // update_best_move_or_insert_into_movelist_wmp. Reset at start of each
  // wordmap_gen and folded into the cache on exit.
  Equity current_anchor_max_equity;
  const Board *board;
  LetterDistribution ld;
  MoveList *move_list;
  AnchorHeap anchor_heap;
  Equity tile_scores[MACHINE_LETTER_MAX_VALUE];
  Equity full_rack_descending_tile_scores[RACK_SIZE];
  Equity descending_tile_scores[RACK_SIZE];
  Equity descending_tile_scores_copy[RACK_SIZE];
  WMPMoveGen wmp_move_gen;
  uint64_t rack_cross_set;
  bool target_word_full_rack_existence[RACK_SIZE + 1];

  Equity best_leaves[RACK_SIZE + 1];

  MachineLetter playthrough_marked[BOARD_DIM];
} MoveGen;

typedef struct MoveGenArgs {
  const Game *game;
  move_record_t move_record_type;
  move_sort_t move_sort_type;
  const KWG *override_kwg;
  Equity eq_margin_movegen;

  // Movegen for inferences after plays will need a target equity value to skip
  // anchors that can't surpass it. Callers may either pass a baseline equity
  // with a separate margin in eq_margin_movegen, or combine them into
  // target_equity and pass eq_margin_movegen=0 (the inference code does this
  // for tile placements). Defaults to EQUITY_MAX_VALUE for non-inference mode.
  Equity target_equity;

  // Movegen for inferences after exchanges will need to know the number of
  // tiles exchanged so that it can determine the target equity value after
  // generating exchanges, and then skip anchors that can't surpass that value
  // by eq_margin_movegen. This field does not itself trigger an early return
  // from move generation based on alternate exchange sizes. Value is
  // UNSET_LEAVE_SIZE for non-exchange scenarios.
  int target_leave_size_for_exchange_cutoff;
  int thread_index;
  MoveList *move_list;
  // Output: bitvector of machine letters that appear in any valid move.
  // Only used with MOVE_RECORD_TILES_PLAYED. Caller provides pointer; callee
  // writes result. Bit i set means machine letter i is playable.
  uint64_t *tiles_played_bv;
  // Input: initial set of known-playable tiles for MOVE_RECORD_TILES_PLAYED.
  // Movegen ORs further discoveries in. Default 0 (no known tiles).
  uint64_t initial_tiles_bv;
  // Optional: if non-NULL, generate moves as if the on-turn player held this
  // rack instead of the rack stored on the game's Player object. The bag is
  // NOT modified, and consistency with the bag is NOT checked — caller is
  // responsible for ensuring the override is sensible. Used for benchmarking
  // and analysis where you want to ask "what would generate_moves return for
  // arbitrary rack X at this board state?" without disturbing the game's
  // actual player rack. Defaults to NULL (uses player's rack).
  const Rack *override_rack;
} MoveGenArgs;

void gen_destroy_cache(void);

// If override_kwg is NULL, the full KWG for the on-turn player is used,
// but if it is nonnull, override_kwg is used. The only use case for this
// so far is using a reduced wordlist kwg (done with wordprune) for endgame
// solving.
void generate_moves(const MoveGenArgs *args);

MoveGen *get_movegen(int thread_index);

void gen_load_position(MoveGen *gen, const MoveGenArgs *args);

void gen_look_up_leaves_and_record_exchanges(MoveGen *gen);

void gen_shadow(MoveGen *gen);

#endif