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
#include "../ent/word_info_table.h"
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
  // Read-only view into the board's lanes for the current cross index;
  // refreshed by gen_load_position each call.
  // Direct views into the board (not copies): board_lanes points at all lane
  // squares for the cross index, row_squares at the current row/dir within it.
  // Square grew large enough that copying a row per anchor cost more than the
  // locality it bought, so move generation reads the board in place.
  const Square *board_lanes;
  const Square *row_squares;
  // Parallel WIT block data for the current lane (see Board.wit_block_rows),
  // set alongside row_squares. NULL when no word info table is loaded.
  const uint32_t *const *wit_row_lane;
  const uint8_t *wit_len_lane;
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
  // (test-only set_klv_leave_value path), leave-derived caches (the subrack
  // cache) must be invalidated even though the KLV pointer is unchanged.
  uint64_t klv_mutation_counter_at_load;
  // Instance fingerprints of the KLV and WMP captured at the last
  // gen_load_position call. The MoveGen cache is pooled per thread and
  // outlives the Configs the engine creates and destroys; when a Config is
  // freed and another loaded, the allocator can hand the new KLV/WMP the freed
  // one's struct address (ABA) -- even for the same lexicon -- while its
  // internal arrays are reallocated elsewhere. A KLV/WMP pointer (or lexicon
  // name) comparison reads "unchanged" and keeps stale leave_values / dangling
  // wmp_entry pointers cached. The fingerprint hashes the internal array
  // addresses, so it changes whenever the backing data is reloaded.
  uint64_t klv_instance_fp_at_load;
  uint64_t wmp_instance_fp_at_load;
  const RackInfoTable *rack_info_table;
  // Optional precomputed word info table (loaded with -wit). When non-NULL,
  // wmp_move_gen prunes subracks whose letters cannot appear in any word
  // containing the playthrough blocks. NULL disables the optimization.
  const WordInfoTable *word_info_table;
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
  const Board *board;
  LetterDistribution ld;
  // Whether ld fits a BitRack: <= BIT_RACK_MAX_ALPHABET_SIZE machine letters
  // (4-bit letter index) and <= 15 of any one letter, blanks included (4-bit
  // per-letter count) -- see bit_rack_is_compatible_with_ld. The RIT cache,
  // KLV-leaves cache, and WMP all key on a BitRack, so all three are disabled
  // when this is false. Set in gen_load_position.
  bool bit_rack_compatible;
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
  MoveList *move_list;
  // Output: bitvector of machine letters that appear in any valid move.
  // Only used with MOVE_RECORD_TILES_PLAYED. Caller provides pointer; callee
  // writes result. Bit i set means machine letter i is playable.
  uint64_t *tiles_played_bv;
  // Input: initial set of known-playable tiles for MOVE_RECORD_TILES_PLAYED.
  // Movegen ORs further discoveries in. Default 0 (no known tiles).
  uint64_t initial_tiles_bv;
} MoveGenArgs;

void gen_destroy_cache(void);

// If override_kwg is NULL, the full KWG for the on-turn player is used,
// but if it is nonnull, override_kwg is used. The only use case for this
// so far is using a reduced wordlist kwg (done with wordprune) for endgame
// solving.
void generate_moves(const MoveGenArgs *args);

MoveGen *get_movegen(void);

void gen_load_position(MoveGen *gen, const MoveGenArgs *args);

void gen_look_up_leaves_and_record_exchanges(MoveGen *gen);

void gen_shadow(MoveGen *gen);

#endif