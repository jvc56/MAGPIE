#include "census_opening.h"

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/player.h"
#include "census.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct OpeningAnchor {
  const Square *squares;
  int col;
  int dir;
} OpeningAnchor;

typedef struct OpeningCensus {
  const CensusLattice *lattice;
  const KWG *kwg;
  const LetterDistribution *ld;
  const uint8_t *unseen;
  const Equity *word_penalties;
  const Equity *letter_penalties;
  Equity *scores;
  Equity bingo_bonus;
  int num_blanks;
  int groups;
  int group;
  int first_ml;
  int num_anchors;
  OpeningAnchor anchors[BOARD_DIM * BOARD_DIM * 2];
  uint8_t remaining[MAX_ALPHABET_SIZE];
  MachineLetter word[RACK_SIZE];
  uint64_t plays;
} OpeningCensus;

// Enumerating opening words separately leaves the nonopening generator's hot
// path unchanged. Every placement of a word contributes its adjusted score;
// taking a raw-score maximum before adjusting would lose the best placement.
static void opening_record(OpeningCensus *gen, int length) {
  for (int anchor_idx = 0; anchor_idx < gen->num_anchors; anchor_idx++) {
    const OpeningAnchor *anchor = &gen->anchors[anchor_idx];
    const int first_start =
        anchor->col >= length ? anchor->col - length + 1 : 0;
    for (int start = first_start;
         start <= anchor->col && start + length <= BOARD_DIM; start++) {
      Equity letter_scores[RACK_SIZE];
      Equity raw_score = 0;
      Equity adjustment = 0;
      int word_multiplier = 1;
      for (int tile_idx = 0; tile_idx < length; tile_idx++) {
        const int col = start + tile_idx;
        const MachineLetter ml = gen->word[tile_idx];
        const BonusSquare bonus =
            square_get_bonus_square(&anchor->squares[col]);
        letter_scores[tile_idx] = ld_get_score(gen->ld, ml) *
                                  bonus_square_get_letter_multiplier(bonus);
        raw_score += letter_scores[tile_idx];
        word_multiplier *= bonus_square_get_word_multiplier(bonus);
        if (ld_get_is_vowel(gen->ld, ml)) {
          const int penalty_idx = (anchor->dir * BOARD_DIM) + col;
          adjustment += gen->word_penalties[penalty_idx] +
                        gen->letter_penalties[penalty_idx];
        }
      }
      Equity score = (raw_score * word_multiplier) + adjustment;
      if (length == RACK_SIZE) {
        score += gen->bingo_bonus;
      }
      CensusPlacedTile placed[RACK_SIZE];
      for (int tile_idx = 0; tile_idx < length; tile_idx++) {
        placed[tile_idx] = (CensusPlacedTile){
            .ml = gen->word[tile_idx],
            .blank_drop = letter_scores[tile_idx] * word_multiplier};
      }
      // Assigned blanks have the vowel identity of the letter they represent,
      // so blanking changes face score but not this placement adjustment.
      census_record_blank_variants_shared(gen->lattice, gen->scores, score,
                                          placed, length, gen->unseen,
                                          gen->num_blanks);
      gen->plays++;
    }
  }
}

static void opening_walk(OpeningCensus *gen, uint32_t node_index, int depth) {
  for (;; node_index++) {
    const uint32_t node = kwg_node(gen->kwg, node_index);
    const MachineLetter ml = kwg_node_tile(node);
    const bool in_group = (depth != 0 || ml == gen->first_ml) &&
                          (depth != 1 || ml % gen->groups == gen->group);
    const MachineLetter rack_ml =
        gen->remaining[ml] > 0 ? ml : BLANK_MACHINE_LETTER;
    if (in_group && gen->remaining[rack_ml] > 0) {
      gen->remaining[rack_ml]--;
      gen->word[depth] = ml;
      if (kwg_node_accepts(node) && (depth > 0 || gen->group == 0)) {
        opening_record(gen, depth + 1);
      }
      const uint32_t child = kwg_node_arc_index(node);
      if (child != 0 && depth + 1 < RACK_SIZE) {
        opening_walk(gen, child, depth + 1);
      }
      gen->remaining[rack_ml]++;
    }
    if (kwg_node_is_end(node)) {
      return;
    }
  }
}

uint64_t census_opening_build_claimed(const Game *game,
                                      const CensusLattice *lattice,
                                      const uint8_t *unseen_tally,
                                      Equity *scores, int second_tile_groups,
                                      _Atomic int *next_unit) {
  const Board *board = game_get_board(game);
  assert(board_get_tiles_played(board) == 0);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  OpeningCensus gen = {
      .lattice = lattice,
      .kwg = player_get_kwg(game_get_player(game, 0)),
      .ld = ld,
      .unseen = unseen_tally,
      .word_penalties = board_get_opening_move_word_penalties(board),
      .letter_penalties = board_get_opening_move_letter_penalties(board),
      .scores = scores,
      .bingo_bonus = game_get_bingo_bonus(game),
      .num_blanks = unseen_tally[BLANK_MACHINE_LETTER],
      .groups = second_tile_groups};
  for (int ml = 0; ml < ld_size; ml++) {
    gen.remaining[ml] = unseen_tally[ml];
  }
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), 0);
  const Square *lanes = board_get_readonly_lanes(board, cross_index);
  for (int dir = 0; dir < 2; dir++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      const Square *squares = board_get_row_cache(lanes, row, dir);
      for (int col = 0; col < BOARD_DIM; col++) {
        if (square_get_anchor(&squares[col])) {
          gen.anchors[gen.num_anchors++] =
              (OpeningAnchor){.squares = squares, .col = col, .dir = dir};
        }
      }
    }
  }
  const uint32_t root = kwg_get_dawg_root_node_index(gen.kwg);
  if (root == 0) {
    return 0;
  }
  const int units = ld_size * second_tile_groups;
  for (int unit_idx =
           atomic_fetch_add_explicit(next_unit, 1, memory_order_relaxed);
       unit_idx < units; unit_idx = atomic_fetch_add_explicit(
                             next_unit, 1, memory_order_relaxed)) {
    gen.first_ml = unit_idx / second_tile_groups;
    gen.group = unit_idx % second_tile_groups;
    opening_walk(&gen, root, 0);
  }
  return gen.plays;
}
