#ifndef MOVE_GEN_H
#define MOVE_GEN_H

#include "../def/move_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../impl/wmp_move_gen.h"

typedef struct UnrestrictedMultiplier {
  uint8_t multiplier;
  uint8_t column;
} UnrestrictedMultiplier;

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
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
  int number_of_plays;
  int move_sort_type;
  move_record_t move_record_type;
  int number_of_tiles_in_bag;
  int player_index;
  int bingo_bonus;
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
  // This field is only used for the MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST
  // record type
  Equity best_move_equity_or_score;
  Equity max_equity_diff;

  MachineLetter strip[(MOVE_MAX_TILES)];
  MachineLetter exchange_strip[(MOVE_MAX_TILES)];
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
  uint64_t rack_cross_set;
  int number_of_letters_on_rack;
  Equity full_rack_descending_tile_scores[WORD_ALIGNING_RACK_SIZE];
  Equity descending_tile_scores[WORD_ALIGNING_RACK_SIZE];
  Equity descending_tile_scores_copy[WORD_ALIGNING_RACK_SIZE];
  Equity best_leaves[(RACK_SIZE)];

  AnchorHeap anchor_heap;
  LetterDistribution ld;

  // Include space for blank letters so their scores can be added without
  // checking whether tiles are blanked.
  Equity tile_scores[MAX_ALPHABET_SIZE + BLANK_MASK];

  // Owned by the caller
  const Board *board;
  const KLV *klv;
  const KWG *kwg;
  MoveList *move_list;

  WMPMoveGen wmp_move_gen;
  // Number of playthrough blocks used by the by current move of recursive_gen
  // given the rightmost column of the move. Set per anchor because it depends
  // on the anchor column.
  uint8_t num_playthrough_blocks[BOARD_DIM];
  uint8_t max_playthrough_blocks;
  // Used by wordmap_gen to prepare WMP-generated plays for recording.
  uint8_t playthrough_marked[BOARD_DIM];
} MoveGen;

typedef struct MoveGenArgs {
  Game *game;
  move_record_t move_record_type;
  move_sort_t move_sort_type;
  Equity max_equity_diff;
  int thread_index;
  MoveList *move_list;
  const KWG *override_kwg;
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