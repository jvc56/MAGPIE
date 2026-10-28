#ifndef MOVE_GEN_BASE_H
#define MOVE_GEN_BASE_H

// The move generator's state, and the helpers that read the lane being
// searched and record the plays found there. move_gen.c generates classic
// plays with them, and the variant generators it dispatches to use them too.

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/lane_shadow_cache.h"
#include "../ent/leave_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/pat_eval.h"
#include "../ent/rack.h"
#include "../ent/rack_info_table.h"
#include "../ent/static_eval.h"
#include "../ent/word_info_table.h"
#include "../util/io_util.h"
#include "wmp_move_gen.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define MOVEGEN_RIT_CACHE_SIZE 64

// Wordsmog's search for the best play with the PAT term (see WordsmogPATGroup
// in movegen_wordsmog.c).
typedef struct WordsmogPATRefine WordsmogPATRefine;

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
  // Lazily populated wmp_entry pointers per subrack, also rack-determined:
  // WMP_ENTRY_UNRESOLVED, a resolved miss (NULL), or a resolved entry.
  // Invalidated when the WMP pointer changes.
  const WMPEntry *wmp_entries[MOVEGEN_SUBRACK_CACHE_ENTRIES];
  uint8_t count_by_size[RACK_SIZE + 1];
} SubrackEnumCacheEntry;

typedef struct UnrestrictedMultiplier {
  uint8_t multiplier;
  uint8_t column;
} UnrestrictedMultiplier;

// A player's movegen budget for one move (-movebudget, -mb1, -mb2): the move
// generations it may spend choosing it -- its inference, simulation and
// solving together. Set on the thread that chooses the move
// (gen_set_movegen_budget), and by each thread that thread starts for the
// move's search, so every generation any of them makes is charged to it.
// Charged a batch at a time, so a move may run a batch per thread past its
// limit before its search sees it spent; a limit of 0 is never spent.
typedef struct MovegenBudget {
  uint64_t limit;
  _Atomic uint64_t spent;
} MovegenBudget;

typedef struct MoveGen {
  // Move generations made on this MoveGen: generate_moves and
  // generate_small_moves_in_lanes calls, by whichever thread holds it. Never
  // reset; see gen_get_movegen_count.
  uint64_t movegen_count;
  // Generations not yet charged to the running command's movegen limit
  // (gen_start_movegen_limit): charged a batch at a time, so the hot path
  // stays a local increment.
  uint64_t limit_uncharged;
  // The move this thread is searching for, if it has a movegen budget, and
  // generations not yet charged to it.
  MovegenBudget *movegen_budget;
  uint64_t budget_uncharged;
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
  // Whether a subrack's leave may take part in the bound that decides if the
  // subrack can still beat the cutoff: only when the bag is not empty and
  // the sort is by equity.
  bool wmp_prune_subracks_by_leave;
  int player_index;
  Equity bingo_bonus;
  // MOVE_RECORD_BEST_SMALL only: added to the ranking value (not the recorded
  // score) of a play of best_small_out_rack_size tiles, i.e. one that uses the
  // whole rack.
  Equity best_small_out_bonus;
  int best_small_out_rack_size;
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
  // In the padding before board_lanes, so no field before it moves.
  bool is_woadwage;
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
  Equity opening_move_word_penalties[(BOARD_DIM) * 2];
  Equity opening_move_letter_penalties[(BOARD_DIM) * 2];
  // PAT evaluation state; disabled (weights NULL) unless the player
  // has PAT weights loaded and the record/sort types use static equity.
  PATEvalContext pat_eval_ctx;
  // pat_eval_lane_penalty_bound for the lane the shadow phase is on, folded
  // into every shadow equity bound recorded from it (zero when disabled).
  Equity pat_lane_penalty_bound;
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
  // The initial complete board block is immutable while shadow explores
  // left starts. Each rightward branch keeps its allowed lengths locally.
  const uint32_t *shadow_position_row;
  int shadow_position_col;
  int shadow_position_length;

  // Used to calculate the maximum score for an anchor. We don't know which
  // tiles will go in which unrestricted squares, so effective multipliers are
  // kept in descending order. The cross-word components need no ordering; they
  // are retained only to rebuild the effective list when the main-word
  // multiplier changes.
  UnrestrictedMultiplier unrestricted_multipliers[WORD_ALIGNING_RACK_SIZE];
  uint16_t descending_effective_letter_multipliers[WORD_ALIGNING_RACK_SIZE];
  uint8_t num_unrestricted_multipliers;
  uint8_t last_word_multiplier;

  // Rightward exploration can reorder the effective array. The append-only
  // backup entries need only their count restored, so they need no copy.
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
  // (including between leavegen generations), leave-derived caches must be
  // invalidated even though the KLV pointer is unchanged.
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
  // The rack_info_table's instance id when it was loaded here, for the same
  // ABA: rit_cache_entries point into the table's entries, and a task that
  // loads another lexicon's table at the freed one's struct address read the
  // old table's entries -- unmapped, a crash; still mapped, the wrong
  // lexicon's bingos and leaves.
  uint64_t rit_instance_id_at_load;
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

  // MOVE_RECORD_BEST_SMALL only: see MoveGenArgs.best_small_canonical_ties.
  // After the hot fields above so their offsets stay stable.
  bool best_small_canonical_ties;
  // MOVE_RECORD_BEST_SMALL only: see MoveGenArgs.lane_shadow_cache. NULL when
  // the generation shadows every lane itself.
  LaneShadowCache *lane_shadow_cache;
  // WMP-only storage at the end keeps every existing hot field's offset.
  // Reset/touch it only for active WMP positions whose rack has a blank.
  WMPBlankWordCache blank_word_cache;
  // How many plays wordsmog's PAT search prices in a generation, INT_MAX for
  // no limit (see player_get_wordsmog_pat_evals), and its state, allocated
  // when first used (see movegen_wordsmog.c).
  int wordsmog_pat_evals;
  WordsmogPATRefine *wordsmog_pat;
} MoveGen;

// Cache getter functions

static inline MachineLetter gen_cache_get_letter(const MoveGen *gen, int col) {
  return square_get_letter(&gen->row_squares[col]);
}

static inline bool gen_cache_get_is_anchor(const MoveGen *gen, int col) {
  return square_get_anchor(&gen->row_squares[col]);
}

static inline int gen_cache_is_empty(const MoveGen *gen, int col) {
  return gen_cache_get_letter(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static inline bool gen_cache_get_is_cross_word(const MoveGen *gen, int col) {
  return square_get_is_cross_word(&gen->row_squares[col]);
}

static inline BonusSquare gen_cache_get_bonus_square(const MoveGen *gen,
                                                     int col) {
  return square_get_bonus_square(&gen->row_squares[col]);
}

static inline uint64_t gen_cache_get_cross_set(const MoveGen *gen, int col) {
  return square_get_cross_set(&gen->row_squares[col]);
}

static inline Equity gen_cache_get_cross_score(const MoveGen *gen, int col) {
  return square_get_cross_score(&gen->row_squares[col]);
}

static inline uint64_t gen_cache_get_left_extension_set(const MoveGen *gen,
                                                        int col) {
  return square_get_left_extension_set(&gen->row_squares[col]);
}

static inline uint64_t gen_cache_get_right_extension_set(const MoveGen *gen,
                                                         int col) {
  return square_get_right_extension_set(&gen->row_squares[col]);
}

static inline Equity gen_get_static_equity(const MoveGen *gen,
                                           const Move *move) {
  return static_eval_get_move_equity_with_leave_value(
      &gen->ld, move, &gen->player_rack, &gen->opponent_rack,
      gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
      pat_eval_context_get_active_classes(&gen->pat_eval_ctx),
      &gen->pat_eval_ctx, gen->board_number_of_tiles_played,
      gen->number_of_tiles_in_bag,
      leave_map_get_current_value(&gen->leave_map));
}

// Best-move recording computes the PAT term lazily: a candidate whose
// equity without it, plus an upper bound on the term, is already strictly
// below the best move's full equity cannot become the best move, so the
// per-move lane rescans run only while the candidate can still contend
// (see gen_pat_contender_equity). The stored equity of a skipped candidate
// is an overestimate used only in a comparison it strictly loses.
static inline bool gen_pat_is_active(const MoveGen *gen) {
  return gen->pat_eval_ctx.weights != NULL;
}

// Static equity without the PAT term. Passes the mover's real active PAT
// classes, so this and gen_get_static_equity differ only by that term.
static inline Equity gen_get_static_equity_without_pat(const MoveGen *gen,
                                                       const Move *move) {
  return static_eval_get_move_equity_with_leave_value(
      &gen->ld, move, &gen->player_rack, &gen->opponent_rack,
      gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
      pat_eval_context_get_active_classes(&gen->pat_eval_ctx), NULL,
      gen->board_number_of_tiles_played, gen->number_of_tiles_in_bag,
      leave_map_get_current_value(&gen->leave_map));
}

static inline const Move *gen_get_readonly_best_move(const MoveGen *gen) {
  return &gen->best_move_and_current_move[gen->best_move_index];
}

// The equity a best-move candidate is recorded with when PAT is active: its
// equity without the term plus the term, which is exact unless the
// candidate cannot reach the best move's equity. Then it is an overestimate
// that still strictly loses the compare (see pat_eval_move_penalty_capped).
static inline Equity gen_pat_contender_equity(const MoveGen *gen,
                                              const Move *move,
                                              const Rack *leave,
                                              Equity equity_without_pat) {
  const Equity best_equity = move_get_equity(gen_get_readonly_best_move(gen));
  Equity floor = EQUITY_MIN_VALUE;
  if (best_equity != EQUITY_INITIAL_VALUE) {
    // The smallest term with which the candidate still reaches the best.
    int64_t needed = (int64_t)best_equity - equity_without_pat;
    if (needed < EQUITY_MIN_VALUE) {
      needed = EQUITY_MIN_VALUE;
    } else if (needed > EQUITY_MAX_VALUE) {
      needed = EQUITY_MAX_VALUE;
    }
    floor = (Equity)needed;
  }
  return equity_without_pat +
         pat_eval_move_penalty_capped(&gen->pat_eval_ctx, move, leave, floor);
}

// The PAT bound, utility correction included, for a WMP anchor's plays:
// the lane's bound on the defense term, and the largest correction a play
// of tiles_to_play tiles scoring at most highest_possible_score can get
// (see pat_eval_utility_bound_for_play). The position-wide correction bound
// assumes the highest-scoring play of any length, which almost no anchor
// can make. Zero when the term is off.
static inline Equity gen_wmp_anchor_pat_bound(const MoveGen *gen, int dir,
                                              int lane, const Anchor *anchor) {
  if (!gen_pat_is_active(gen)) {
    return 0;
  }
  return pat_eval_lane_defense_bound(&gen->pat_eval_ctx, dir, lane) +
         pat_eval_utility_bound_for_play(&gen->pat_eval_ctx,
                                         anchor->tiles_to_play,
                                         anchor->highest_possible_score);
}

static inline Move *gen_get_best_move(MoveGen *gen) {
  return &gen->best_move_and_current_move[gen->best_move_index];
}

static inline Move *gen_get_current_move(MoveGen *gen) {
  return &gen->best_move_and_current_move[gen->best_move_index ^ 1];
}

static inline void gen_switch_best_move_and_current_move(MoveGen *gen) {
  gen->best_move_index ^= 1;
}

static inline void set_play_for_record(Move *move, game_event_t move_type,
                                       int leftstrip, int rightstrip,
                                       Equity score, int start_row,
                                       int start_col, int tiles_played, int dir,
                                       MachineLetter strip[]) {
  move_set_all_except_equity(move, strip, leftstrip, rightstrip, score,
                             start_row, start_col, tiles_played, dir,
                             move_type);
  if (board_is_dir_vertical(dir)) {
    move_set_row_start(move, start_col);
    move_set_col_start(move, start_row);
  }
}

static inline Equity get_move_equity_for_sort_type(const MoveGen *gen,
                                                   const Move *move,
                                                   Equity score) {
  switch (gen->move_sort_type) {
  case MOVE_SORT_EQUITY:
    return gen_get_static_equity(gen, move);
  case MOVE_SORT_SCORE:
    return score;
  default:
    // TODO(olaugh): make move_sort_type an enum and remove default case
    log_fatal("unhandled move sort type: %d", gen->move_sort_type);
  }
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#else
  return score;
#endif
}

static inline void set_small_play_for_record(SmallMove *move,
                                             game_event_t move_type,
                                             int leftstrip, int rightstrip,
                                             Equity score, int start_row,
                                             int start_col, int tiles_played,
                                             int dir, MachineLetter strip[]) {

  small_move_set_all(move, strip, leftstrip, rightstrip, score, start_row,
                     start_col, tiles_played, board_is_dir_vertical(dir),
                     move_type);
}

static inline Equity gen_get_cutoff_equity_or_score(const MoveGen *gen) {
  return gen->cutoff_equity_or_score;
}

// The cutoff never falls between resets: the WMP subrack mask in wordmap_gen
// drops a subrack whose leave bound is below the cutoff when it builds the
// mask, and relies on the cutoff still being at least that high when the walk
// reaches the subrack.
static inline void gen_update_cutoff_equity_or_score(MoveGen *gen) {
  const Equity previous_cutoff = gen->cutoff_equity_or_score;
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_ALL_SMALL:
  case MOVE_RECORD_TILES_PLAYED:
    log_fatal("gen_get_cutoff_equity_or_score called with "
              "MOVE_RECORD_ALL or "
              "MOVE_RECORD_ALL_SMALL");
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
    __builtin_unreachable();
#else
    return;
#endif
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
    gen->cutoff_equity_or_score =
        gen->best_move_equity_or_score - gen->eq_margin_movegen;
    break;
  case MOVE_RECORD_BEST:;
    const Move *move = gen_get_readonly_best_move(gen);
    gen->cutoff_equity_or_score = (gen->move_sort_type == MOVE_SORT_EQUITY)
                                      ? move_get_equity(move)
                                      : move_get_score(move);
    break;
  case MOVE_RECORD_BEST_SMALL:
    // No Move object; best_move_equity_or_score is already the best score.
    gen->cutoff_equity_or_score = gen->best_move_equity_or_score;
    break;
  }
  assert(gen->cutoff_equity_or_score >= previous_cutoff);
  // previous_cutoff is read only by the assert above (compiled out in
  // release).
  (void)previous_cutoff;
}

static inline void
gen_insert_spare_move_within_x_equity_of_best(MoveGen *gen,
                                              Equity move_equity_or_score) {
  if (gen->best_move_equity_or_score == EQUITY_INITIAL_VALUE) {
    gen->best_move_equity_or_score = move_equity_or_score;
    gen_update_cutoff_equity_or_score(gen);
    move_list_insert_spare_move(gen->move_list, move_equity_or_score);
    return;
  }
  Equity cutoff_equity_or_score = gen_get_cutoff_equity_or_score(gen);
  if (move_equity_or_score < cutoff_equity_or_score) {
    return;
  }
  move_list_insert_spare_move(gen->move_list, move_equity_or_score);
  if (move_equity_or_score > gen->best_move_equity_or_score) {
    // The current move is better than the best move, so update the best move to
    // the current move
    gen->best_move_equity_or_score = move_equity_or_score;
    // Update the cutoff now that the best possible equity or score has improved
    gen_update_cutoff_equity_or_score(gen);
    cutoff_equity_or_score = gen_get_cutoff_equity_or_score(gen);
    while (move_list_get_count(gen->move_list) > 0 &&
           move_list_peek_equity(gen->move_list) < cutoff_equity_or_score) {
      move_list_pop_move(gen->move_list);
    }
  }
}

static inline void update_best_move_or_insert_into_movelist(
    MoveGen *gen, int leftstrip, int rightstrip, game_event_t move_type,
    Equity score, int start_row, int start_col, int tiles_played, int dir,
    MachineLetter strip[]) {
  bool need_to_update_best_move_equity_or_score = false;
  Equity move_equity_or_score = 0;
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:;
    Move *move = move_list_get_spare_move(gen->move_list);
    set_play_for_record(move, move_type, leftstrip, rightstrip, score,
                        start_row, start_col, tiles_played, dir, strip);
    move_equity_or_score = get_move_equity_for_sort_type(gen, move, score);
    if (gen->move_record_type == MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST) {
      // This updates the cutoff move internally so no update will be pending
      // afterward.
      gen_insert_spare_move_within_x_equity_of_best(gen, move_equity_or_score);
    } else {
      move_list_insert_spare_move(gen->move_list, move_equity_or_score);
    }
    break;
  case MOVE_RECORD_BEST:;
    Move *current_move = gen_get_current_move(gen);
    set_play_for_record(current_move, move_type, leftstrip, rightstrip, score,
                        start_row, start_col, tiles_played, dir, strip);
    if (gen->move_sort_type == MOVE_SORT_EQUITY && gen_pat_is_active(gen) &&
        !gen->stop_on_threshold) {
      const Equity equity_without_pat =
          gen_get_static_equity_without_pat(gen, current_move);
      move_equity_or_score = gen_pat_contender_equity(
          gen, current_move, &gen->player_rack, equity_without_pat);
    } else {
      move_equity_or_score =
          get_move_equity_for_sort_type(gen, current_move, score);
    }
    move_set_equity(current_move, move_equity_or_score);
    // A best move carried in from an earlier generation (see
    // gen_seed_carried_best_move) is found again; only a better move
    // replaces it.
    if (compare_moves(current_move, gen_get_readonly_best_move(gen), true) ==
        1) {
      need_to_update_best_move_equity_or_score = true;
      gen_switch_best_move_and_current_move(gen);
    }
    break;
  case MOVE_RECORD_ALL_SMALL:;
    SmallMove *small_move = small_move_list_get_spare_move(gen->move_list);
    set_small_play_for_record(small_move, move_type, leftstrip, rightstrip,
                              score, start_row, start_col, tiles_played, dir,
                              strip);
    // small_move doesn't use equity.
    move_list_insert_spare_small_move(gen->move_list);
    break;
  case MOVE_RECORD_TILES_PLAYED:
    // Only record which tile types from the rack appear in valid moves.
    for (int i = leftstrip; i <= rightstrip; i++) {
      MachineLetter tile = strip[i];
      if (tile == PLAYED_THROUGH_MARKER) {
        continue;
      }
      uint8_t ml_val = get_is_blanked(tile) ? 0 : tile;
      // NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift)
      gen->tiles_played_bv |= ((uint64_t)1 << ml_val);
    }
    // Early exit when all rack tile types have been seen
    if ((gen->tiles_played_bv & gen->target_tiles_bv) == gen->target_tiles_bv) {
      gen->threshold_exceeded = true;
    }
    break;
  case MOVE_RECORD_BEST_SMALL:;
    SmallMove *best_sm = small_move_list_get_spare_move(gen->move_list);
    set_small_play_for_record(best_sm, move_type, leftstrip, rightstrip, score,
                              start_row, start_col, tiles_played, dir, strip);
    move_equity_or_score = score;
    if (tiles_played == gen->best_small_out_rack_size) {
      move_equity_or_score += gen->best_small_out_bonus;
    }
    // With canonical ties, equal values go to the larger tiny_move, so the
    // winner does not depend on the order in which anchors are searched. A tie
    // leaves the cutoff unchanged, and the shadow pruning is strict (bound <
    // cutoff), so an equal-valued rival is never pruned. Otherwise the first
    // play found keeps the tie.
    if (move_equity_or_score > gen->best_move_equity_or_score ||
        (move_equity_or_score == gen->best_move_equity_or_score &&
         gen->best_small_canonical_ties &&
         best_sm->tiny_move > gen->move_list->small_moves[0]->tiny_move)) {
      need_to_update_best_move_equity_or_score = true;
      gen->best_move_equity_or_score = move_equity_or_score;
      // Swap spare_small_move into small_moves[0].
      SmallMove *swap = gen->move_list->small_moves[0];
      gen->move_list->small_moves[0] = gen->move_list->spare_small_move;
      gen->move_list->spare_small_move = swap;
      gen->move_list->count = 1;
    }
    break;
  }

  if (need_to_update_best_move_equity_or_score) {
    gen_update_cutoff_equity_or_score(gen);
  }

  if (gen->stop_on_threshold &&
      move_equity_or_score > gen->target_equity_cutoff) {
    gen->threshold_exceeded = true;
  }
}

static inline void record_tile_placement_move(MoveGen *gen, int leftstrip,
                                              int rightstrip,
                                              int main_word_score,
                                              int word_multiplier,
                                              Equity cross_score) {
  const int start_row = gen->current_row_index;
  const int start_col = leftstrip;
  const int tiles_played = gen->tiles_played;

  Equity bingo_bonus = 0;
  if (tiles_played == RACK_SIZE) {
    bingo_bonus = gen->bingo_bonus;
  }

  const Equity score =
      main_word_score * word_multiplier + cross_score + bingo_bonus;

  update_best_move_or_insert_into_movelist(
      gen, leftstrip, rightstrip, GAME_EVENT_TILE_PLACEMENT_MOVE, score,
      start_row, start_col, tiles_played, gen->dir, gen->strip);
}

static inline bool better_play_has_been_found(const MoveGen *gen,
                                              Equity highest_possible_value) {
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_ALL_SMALL:
  case MOVE_RECORD_TILES_PLAYED:
    return false;
    break;
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
    if (gen->best_move_equity_or_score == EQUITY_INITIAL_VALUE) {
      return false;
    }
    break;
  case MOVE_RECORD_BEST_SMALL:
  case MOVE_RECORD_BEST:
    break;
  }
  Equity cutoff = gen_get_cutoff_equity_or_score(gen);
  if (cutoff == EQUITY_MAX_VALUE) {
    return false;
  }
  return highest_possible_value < cutoff;
}

// Index of the lowest set bit of a nonzero bitset, following
// get_single_bit_index's guarded-intrinsic pattern. Undefined for zero, so
// callers loop on `bitset != 0`.
static inline int lowest_set_bit_index(uint32_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctz)
  return __builtin_ctz(bitset);
#else
  int index = 0;
  while ((bitset & 1U) == 0) {
    bitset >>= 1U;
    index++;
  }
  return index;
#endif
}

// lowest_set_bit_index for a 64-bit bitset.
static inline int lowest_set_bit_index_64(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bitset);
#else
  int index = 0;
  while ((bitset & 1U) == 0) {
    bitset >>= 1U;
    index++;
  }
  return index;
#endif
}

static inline bool play_is_nonempty_and_nonduplicate(int tiles_played,
                                                     bool is_unique) {
  return (tiles_played > 1) || ((tiles_played == 1) && is_unique);
}

#endif
