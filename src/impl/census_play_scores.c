#include "census_play_scores.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// A placed tile's part of the play's score: its face value times its letter
// multiplier, and the word multiplier of its square if it forms a cross word
// (0 if not). Blanking it drops the score by letter_score times the main
// word's multiplier plus cross_word_multiplier.
typedef struct PlayScoresPlacedTile {
  MachineLetter ml;
  Equity letter_score;
  int cross_word_multiplier;
} PlayScoresPlacedTile;

typedef struct PlayScoresGen {
  const CensusLattice *lattice;
  Equity *play_scores;
  const uint8_t *unseen_tally;
  const KWG *kwg;
  const Square *row_squares;
  const LetterDistribution *ld;
  Equity bingo_bonus;
  int num_blanks;
  int dir;
  int current_anchor_col;
  int last_anchor_col;
  uint64_t anchor_right_extension_set;
  // what is left to place: the pool, each letter capped at RACK_SIZE
  uint16_t rack[MAX_ALPHABET_SIZE];
  int tiles_left;
  // every letter the pool can place, all of them if it holds a blank
  uint64_t rack_cross_set;
  int tiles_played;
  PlayScoresPlacedTile placed[RACK_SIZE];
  uint64_t number_of_plays;
} PlayScoresGen;

// The index of the lowest set bit of a nonzero bitset.
static inline int play_scores_gen_lowest_set_bit(uint64_t bitset) {
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

static inline MachineLetter play_scores_gen_get_letter(const PlayScoresGen *gen,
                                                       int col) {
  return square_get_letter(&gen->row_squares[col]);
}

static inline bool play_scores_gen_is_empty(const PlayScoresGen *gen, int col) {
  return play_scores_gen_get_letter(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static inline Equity
play_scores_gen_board_letter_score(const PlayScoresGen *gen, MachineLetter ml) {
  if (get_is_blanked(ml)) {
    return ld_get_score(gen->ld, BLANK_MACHINE_LETTER);
  }
  return ld_get_score(gen->ld, ml);
}

static void play_scores_gen_record(PlayScoresGen *gen, Equity main_word_score,
                                   int word_multiplier, Equity cross_score) {
  gen->number_of_plays++;
  Equity score = main_word_score * word_multiplier + cross_score;
  if (gen->tiles_played == RACK_SIZE) {
    score += gen->bingo_bonus;
  }
  CensusPlacedTile placed[RACK_SIZE];
  for (int placed_idx = 0; placed_idx < gen->tiles_played; placed_idx++) {
    const PlayScoresPlacedTile *tile = &gen->placed[placed_idx];
    placed[placed_idx] = (CensusPlacedTile){
        .ml = (uint8_t)tile->ml,
        .blank_drop = tile->letter_score *
                      (word_multiplier + tile->cross_word_multiplier)};
  }
  census_record_blank_variants(gen->lattice, gen->play_scores, score, placed,
                               gen->tiles_played, gen->unseen_tally,
                               gen->num_blanks);
}

static void play_scores_gen_go_on(PlayScoresGen *gen, int current_col,
                                  MachineLetter ml, uint32_t new_node_index,
                                  bool accepts, Equity main_word_score,
                                  int word_multiplier, Equity cross_score);

static void play_scores_gen_recursive(PlayScoresGen *gen, int col,
                                      uint32_t node_index,
                                      Equity main_word_score,
                                      int word_multiplier, Equity cross_score) {
  const MachineLetter current_letter = play_scores_gen_get_letter(gen, col);
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const MachineLetter raw = get_unblanked_machine_letter(current_letter);
    uint32_t next_node_index = 0;
    bool accepts = false;
    const uint64_t list_tiles = kwg_get_sibling_tiles(gen->kwg)[node_index];
    const uint64_t raw_bit = (uint64_t)1 << raw;
    if ((list_tiles & raw_bit) != 0) {
      const uint32_t node =
          kwg_list_node(gen->kwg, node_index, list_tiles, raw_bit);
      next_node_index = kwg_node_arc_index_prefetch(node, gen->kwg);
      accepts = kwg_node_accepts(node);
    }
    play_scores_gen_go_on(gen, col, current_letter, next_node_index, accepts,
                          main_word_score, word_multiplier, cross_score);
    return;
  }
  if (gen->tiles_left == 0 || gen->tiles_played == RACK_SIZE) {
    return;
  }
  const Square *square = &gen->row_squares[col];
  uint64_t possible_letters_here =
      square_get_cross_set(square) & square_get_left_extension_set(square);
  if (gen->tiles_played == 0 && col == gen->current_anchor_col + 1) {
    possible_letters_here &= gen->anchor_right_extension_set;
  }
  const uint64_t list_tiles = kwg_get_sibling_tiles(gen->kwg)[node_index];
  uint64_t placeable =
      possible_letters_here & list_tiles & gen->rack_cross_set & ~(uint64_t)1;
  for (; placeable != 0; placeable &= placeable - 1) {
    const MachineLetter ml =
        (MachineLetter)play_scores_gen_lowest_set_bit(placeable);
    // A real tile when the pool has one, else a blank: each word is spelled
    // once, and census_record_blank_variants fills in its other spellings.
    MachineLetter rack_ml = ml;
    if (gen->rack[ml] == 0) {
      if (gen->rack[BLANK_MACHINE_LETTER] == 0) {
        continue;
      }
      rack_ml = BLANK_MACHINE_LETTER;
    }
    const uint32_t node =
        kwg_list_node(gen->kwg, node_index, list_tiles, (uint64_t)1 << ml);
    const uint32_t next_node_index =
        kwg_node_arc_index_prefetch(node, gen->kwg);
    gen->rack[rack_ml]--;
    gen->tiles_left--;
    play_scores_gen_go_on(gen, col, ml, next_node_index, kwg_node_accepts(node),
                          main_word_score, word_multiplier, cross_score);
    gen->tiles_left++;
    gen->rack[rack_ml]++;
  }
}

static void play_scores_gen_go_on(PlayScoresGen *gen, int current_col,
                                  MachineLetter ml, uint32_t new_node_index,
                                  bool accepts, Equity main_word_score,
                                  int word_multiplier, Equity cross_score) {
  const Square *square = &gen->row_squares[current_col];
  const bool square_is_empty = play_scores_gen_is_empty(gen, current_col);
  // The score counts every placed tile at face value, a blank's included;
  // census_record_blank_variants takes the blanks' values back off.
  Equity letter_score;
  int this_word_multiplier = 1;
  bool placed_here = false;
  if (square_is_empty) {
    const BonusSquare bonus_square = square_get_bonus_square(square);
    this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    letter_score = ld_get_score(gen->ld, ml) *
                   bonus_square_get_letter_multiplier(bonus_square);
    int cross_word_multiplier = 0;
    if (square_get_is_cross_word(square)) {
      cross_score += (letter_score + square_get_cross_score(square)) *
                     this_word_multiplier;
      cross_word_multiplier = this_word_multiplier;
    }
    gen->placed[gen->tiles_played] =
        (PlayScoresPlacedTile){.ml = ml,
                               .letter_score = letter_score,
                               .cross_word_multiplier = cross_word_multiplier};
    gen->tiles_played++;
    placed_here = true;
  } else {
    letter_score = play_scores_gen_board_letter_score(gen, ml);
  }
  main_word_score += letter_score;
  word_multiplier *= this_word_multiplier;

  if (current_col <= gen->current_anchor_col) {
    const bool no_letter_directly_left =
        current_col == 0 || play_scores_gen_is_empty(gen, current_col - 1);
    if (accepts && no_letter_directly_left && gen->tiles_played > 0) {
      play_scores_gen_record(gen, main_word_score, word_multiplier,
                             cross_score);
    }
    if (new_node_index != 0) {
      if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
        play_scores_gen_recursive(gen, current_col - 1, new_node_index,
                                  main_word_score, word_multiplier,
                                  cross_score);
      }
      if (gen->tiles_played != 0 ||
          (gen->anchor_right_extension_set & gen->rack_cross_set) != 0) {
        const uint32_t separation_node_index = kwg_get_next_node_index(
            gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
        if (separation_node_index != 0 && no_letter_directly_left &&
            gen->current_anchor_col < BOARD_DIM - 1) {
          play_scores_gen_recursive(gen, gen->current_anchor_col + 1,
                                    separation_node_index, main_word_score,
                                    word_multiplier, cross_score);
        }
      }
    }
  } else {
    const bool no_letter_directly_right =
        current_col == BOARD_DIM - 1 ||
        play_scores_gen_is_empty(gen, current_col + 1);
    if (accepts && no_letter_directly_right && gen->tiles_played > 0) {
      play_scores_gen_record(gen, main_word_score, word_multiplier,
                             cross_score);
    }
    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      play_scores_gen_recursive(gen, current_col + 1, new_node_index,
                                main_word_score, word_multiplier, cross_score);
    }
  }
  if (placed_here) {
    gen->tiles_played--;
  }
}

void census_get_unseen_tally(const Game *game, uint8_t *unseen_tally) {
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen_tally[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter ml = board_get_letter(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      const MachineLetter tile = get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml;
      if (unseen_tally[tile] > 0) {
        unseen_tally[tile]--;
      }
    }
  }
}

uint64_t census_play_scores_build(const Game *game,
                                  const CensusLattice *lattice,
                                  const uint8_t *unseen_tally,
                                  Equity *play_scores) {
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const int player_index = 0;
  const Player *player = game_get_player(game, player_index);
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), player_index);

  PlayScoresGen gen;
  memset(&gen, 0, sizeof(gen));
  gen.lattice = lattice;
  gen.play_scores = play_scores;
  gen.unseen_tally = unseen_tally;
  gen.kwg = player_get_kwg(player);
  gen.ld = ld;
  gen.bingo_bonus = game_get_bingo_bonus(game);
  for (int ml = 0; ml < ld_size; ml++) {
    const int count =
        unseen_tally[ml] < RACK_SIZE ? unseen_tally[ml] : RACK_SIZE;
    gen.rack[ml] = (uint16_t)count;
    gen.tiles_left += count;
    if (count > 0) {
      gen.rack_cross_set |= (uint64_t)1 << ml;
    }
  }
  gen.num_blanks = gen.rack[BLANK_MACHINE_LETTER];
  if (gen.num_blanks > 0) {
    gen.rack_cross_set = TRIVIAL_CROSS_SET;
  }

  uint8_t row_number_of_anchors[BOARD_DIM * 2];
  board_load_number_of_row_anchors_cache(board, row_number_of_anchors);
  const Square *lanes = board_get_readonly_lanes(board, cross_index);
  const uint32_t root_node_index = kwg_get_root_node_index(gen.kwg);
  for (int dir = 0; dir < 2; dir++) {
    gen.dir = dir;
    for (int row = 0; row < BOARD_DIM; row++) {
      if (row_number_of_anchors[BOARD_DIM * dir + row] == 0) {
        continue;
      }
      gen.row_squares = board_get_row_cache(lanes, row, dir);
      int last_anchor_col = BOARD_DIM;
      for (int col = 0; col < BOARD_DIM; col++) {
        if (!square_get_anchor(&gen.row_squares[col])) {
          continue;
        }
        gen.current_anchor_col = col;
        gen.last_anchor_col = last_anchor_col;
        gen.anchor_right_extension_set =
            square_get_right_extension_set(&gen.row_squares[col]);
        gen.tiles_played = 0;
        play_scores_gen_recursive(&gen, col, root_node_index, 0, 1, 0);
        last_anchor_col = col;
        if (!play_scores_gen_is_empty(&gen, col)) {
          last_anchor_col++;
        }
      }
    }
  }
  return gen.number_of_plays;
}
