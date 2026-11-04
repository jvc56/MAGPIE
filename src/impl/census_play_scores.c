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

typedef struct PlayScoresGen {
  const KWG *kwg;
  const LetterDistribution *ld;
  census_play_recorder_func_t record;
  void *record_context;
  Equity bingo_bonus;
  int current_anchor_col;
  int last_anchor_col;
  uint64_t anchor_right_extension_set;
  // what is left to place: the pool, each letter capped at RACK_SIZE
  uint16_t rack[MAX_ALPHABET_SIZE];
  int tiles_left;
  // every letter the pool can place, all of them if it holds a blank
  uint64_t rack_cross_set;
  // The play so far: its lane, its leftmost column (the anchor's until it
  // extends left), its placed tiles and their count. The rightmost column is
  // the anchor's while the play extends left, and the recorded square's after.
  CensusPlay play;
  CensusPlayTile tiles[RACK_SIZE];
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
  return square_get_letter(&gen->play.lane[col]);
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

static void play_scores_gen_record(PlayScoresGen *gen, int rightstrip,
                                   Equity main_word_score, int word_multiplier,
                                   Equity cross_score) {
  gen->number_of_plays++;
  Equity score = main_word_score * word_multiplier + cross_score;
  if (gen->play.tiles_played == RACK_SIZE) {
    score += gen->bingo_bonus;
  }
  gen->play.rightstrip = rightstrip;
  gen->play.word_multiplier = word_multiplier;
  gen->play.score = score;
  gen->record(&gen->play, gen->record_context);
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
  if (gen->tiles_left == 0 || gen->play.tiles_played == RACK_SIZE) {
    return;
  }
  const Square *square = &gen->play.lane[col];
  uint64_t possible_letters_here =
      square_get_cross_set(square) & square_get_left_extension_set(square);
  if (gen->play.tiles_played == 0 && col == gen->current_anchor_col + 1) {
    possible_letters_here &= gen->anchor_right_extension_set;
  }
  const uint64_t list_tiles = kwg_get_sibling_tiles(gen->kwg)[node_index];
  uint64_t placeable =
      possible_letters_here & list_tiles & gen->rack_cross_set & ~(uint64_t)1;
  for (; placeable != 0; placeable &= placeable - 1) {
    const MachineLetter ml =
        (MachineLetter)play_scores_gen_lowest_set_bit(placeable);
    // A real tile when the pool has one, else a blank: each word is spelled
    // once, and the recorder chooses its blanks.
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
  const Square *square = &gen->play.lane[current_col];
  const bool square_is_empty = play_scores_gen_is_empty(gen, current_col);
  // The score counts every placed tile at face value, a blank's included; the
  // recorder takes the blanks' values back off.
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
    gen->tiles[gen->play.tiles_played] =
        (CensusPlayTile){.ml = ml,
                         .col = current_col,
                         .letter_score = letter_score,
                         .cross_word_multiplier = cross_word_multiplier};
    gen->play.tiles_played++;
    placed_here = true;
  } else {
    letter_score = play_scores_gen_board_letter_score(gen, ml);
  }
  main_word_score += letter_score;
  word_multiplier *= this_word_multiplier;

  if (current_col <= gen->current_anchor_col) {
    gen->play.leftstrip = current_col;
    const bool no_letter_directly_left =
        current_col == 0 || play_scores_gen_is_empty(gen, current_col - 1);
    if (accepts && no_letter_directly_left && gen->play.tiles_played > 0) {
      play_scores_gen_record(gen, gen->current_anchor_col, main_word_score,
                             word_multiplier, cross_score);
    }
    if (new_node_index != 0) {
      if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
        play_scores_gen_recursive(gen, current_col - 1, new_node_index,
                                  main_word_score, word_multiplier,
                                  cross_score);
      }
      if (gen->play.tiles_played != 0 ||
          (gen->anchor_right_extension_set & gen->rack_cross_set) != 0) {
        const uint32_t separation_node_index = kwg_get_next_node_index(
            gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
        if (separation_node_index != 0 && no_letter_directly_left &&
            gen->current_anchor_col < BOARD_DIM - 1) {
          // The search to the left moved the play's left end; the plays to
          // the right start here.
          gen->play.leftstrip = current_col;
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
    if (accepts && no_letter_directly_right && gen->play.tiles_played > 0) {
      play_scores_gen_record(gen, current_col, main_word_score, word_multiplier,
                             cross_score);
    }
    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      play_scores_gen_recursive(gen, current_col + 1, new_node_index,
                                main_word_score, word_multiplier, cross_score);
    }
  }
  if (placed_here) {
    gen->play.tiles_played--;
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

uint64_t census_play_scores_search(const Game *game, int player_index,
                                   const uint8_t *unseen_tally,
                                   census_play_recorder_func_t record,
                                   void *record_context) {
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const Player *player = game_get_player(game, player_index);
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), player_index);

  PlayScoresGen gen;
  memset(&gen, 0, sizeof(gen));
  gen.kwg = player_get_kwg(player);
  gen.ld = ld;
  gen.record = record;
  gen.record_context = record_context;
  gen.bingo_bonus = game_get_bingo_bonus(game);
  gen.play.tiles = gen.tiles;
  for (int ml = 0; ml < ld_size; ml++) {
    const int count =
        unseen_tally[ml] < RACK_SIZE ? unseen_tally[ml] : RACK_SIZE;
    gen.rack[ml] = (uint16_t)count;
    gen.tiles_left += count;
    if (count > 0) {
      gen.rack_cross_set |= (uint64_t)1 << ml;
    }
  }
  if (gen.rack[BLANK_MACHINE_LETTER] > 0) {
    gen.rack_cross_set = TRIVIAL_CROSS_SET;
  }

  uint8_t row_number_of_anchors[BOARD_DIM * 2];
  board_load_number_of_row_anchors_cache(board, row_number_of_anchors);
  const Square *lanes = board_get_readonly_lanes(board, cross_index);
  const uint32_t root_node_index = kwg_get_root_node_index(gen.kwg);
  for (int dir = 0; dir < 2; dir++) {
    gen.play.dir = dir;
    for (int row = 0; row < BOARD_DIM; row++) {
      if (row_number_of_anchors[BOARD_DIM * dir + row] == 0) {
        continue;
      }
      gen.play.lane_index = row;
      gen.play.lane = board_get_row_cache(lanes, row, dir);
      int last_anchor_col = BOARD_DIM;
      for (int col = 0; col < BOARD_DIM; col++) {
        if (!square_get_anchor(&gen.play.lane[col])) {
          continue;
        }
        gen.current_anchor_col = col;
        gen.last_anchor_col = last_anchor_col;
        gen.anchor_right_extension_set =
            square_get_right_extension_set(&gen.play.lane[col]);
        gen.play.tiles_played = 0;
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

typedef struct PlayScoresRecordContext {
  const CensusLattice *lattice;
  Equity *play_scores;
  const uint8_t *unseen_tally;
  int num_blanks;
} PlayScoresRecordContext;

// Raises play_scores for the multiset the play places, and for each of its
// spellings with blanks.
static void play_scores_record(const CensusPlay *play, void *context) {
  const PlayScoresRecordContext *record_context = context;
  CensusPlacedTile placed[RACK_SIZE];
  for (int tile_idx = 0; tile_idx < play->tiles_played; tile_idx++) {
    placed[tile_idx] = (CensusPlacedTile){
        .ml = (uint8_t)play->tiles[tile_idx].ml,
        .blank_drop = census_play_blank_drop(play, tile_idx)};
  }
  census_record_blank_variants(record_context->lattice,
                               record_context->play_scores, play->score, placed,
                               play->tiles_played, record_context->unseen_tally,
                               record_context->num_blanks);
}

uint64_t census_play_scores_build(const Game *game,
                                  const CensusLattice *lattice,
                                  const uint8_t *unseen_tally,
                                  Equity *play_scores) {
  const int num_blanks = unseen_tally[BLANK_MACHINE_LETTER] < RACK_SIZE
                             ? unseen_tally[BLANK_MACHINE_LETTER]
                             : RACK_SIZE;
  PlayScoresRecordContext record_context = {.lattice = lattice,
                                            .play_scores = play_scores,
                                            .unseen_tally = unseen_tally,
                                            .num_blanks = num_blanks};
  return census_play_scores_search(game, 0, unseen_tally, play_scores_record,
                                   &record_context);
}
