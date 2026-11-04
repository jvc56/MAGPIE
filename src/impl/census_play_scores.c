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
#include <stdatomic.h>
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
  // One of the two is set (see BlankVariantContext in census.c).
  Equity *play_scores;
  // Whether other threads build into play_scores too.
  bool shared;
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
  // The letters the tile placed after tiles_played others may be, which
  // splits a build into units (census_play_scores_build_claimed): all of
  // them in a serial build.
  uint64_t placeable_masks[RACK_SIZE];
  // Whether plays of one tile are recorded, which only one of the units that
  // share a play's first tile does.
  bool record_single_tile_plays;
} PlayScoresGen;

// An anchor of census_play_scores_build_claimed: its lane (a row in one
// direction), its column, the column of the anchor before it in the lane
// (BOARD_DIM if none), and the letters that can be placed on it first.
typedef struct PlayScoresAnchor {
  uint8_t lane;
  uint8_t col;
  uint8_t last_anchor_col;
  bool on_tile;
  // the letters that can be placed on it first, if it is not on a tile
  uint64_t first_tiles;
} PlayScoresAnchor;

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

static inline int play_scores_gen_count_bits(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcountll)
  return __builtin_popcountll(bitset);
#else
  int count = 0;
  for (; bitset != 0; bitset &= bitset - 1) {
    count++;
  }
  return count;
#endif
}

// The letters ml of an alphabet of ld_size letters with ml % groups == group.
static inline uint64_t play_scores_gen_group_mask(int group, int groups,
                                                  int ld_size) {
  uint64_t mask = 0;
  for (int ml = group; ml < ld_size; ml += groups) {
    mask |= (uint64_t)1 << ml;
  }
  return mask;
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
  if (gen->tiles_played == 1 && !gen->record_single_tile_plays) {
    return;
  }
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
  if (gen->shared) {
    census_record_blank_variants_shared(gen->lattice, gen->play_scores, score,
                                        placed, gen->tiles_played,
                                        gen->unseen_tally, gen->num_blanks);
    return;
  }
  census_record_blank_variants(gen->lattice, gen->play_scores, score, placed,
                               gen->tiles_played, gen->unseen_tally,
                               gen->num_blanks);
}

static void play_scores_gen_go_on(PlayScoresGen *gen, int current_col,
                                  MachineLetter ml, uint32_t new_node_index,
                                  bool accepts, Equity main_word_score,
                                  int word_multiplier, Equity cross_score);

// The letters the pool can place on the empty square at col, whose word
// graph node has the children list_tiles.
static inline uint64_t play_scores_gen_placeable(const PlayScoresGen *gen,
                                                 int col, uint64_t list_tiles) {
  const Square *square = &gen->row_squares[col];
  uint64_t possible_letters_here =
      square_get_cross_set(square) & square_get_left_extension_set(square);
  if (gen->tiles_played == 0 && col == gen->current_anchor_col + 1) {
    possible_letters_here &= gen->anchor_right_extension_set;
  }
  return possible_letters_here & list_tiles & gen->rack_cross_set &
         gen->placeable_masks[gen->tiles_played] & ~(uint64_t)1;
}

// Places ml, one of play_scores_gen_placeable's letters, on the empty square
// at col and goes on.
static inline void play_scores_gen_place(PlayScoresGen *gen, int col,
                                         uint32_t node_index,
                                         uint64_t list_tiles, MachineLetter ml,
                                         Equity main_word_score,
                                         int word_multiplier,
                                         Equity cross_score) {
  // A real tile when the pool has one, else a blank: each word is spelled
  // once, and census_record_blank_variants fills in its other spellings.
  MachineLetter rack_ml = ml;
  if (gen->rack[ml] == 0) {
    if (gen->rack[BLANK_MACHINE_LETTER] == 0) {
      return;
    }
    rack_ml = BLANK_MACHINE_LETTER;
  }
  const uint32_t node =
      kwg_list_node(gen->kwg, node_index, list_tiles, (uint64_t)1 << ml);
  const uint32_t next_node_index = kwg_node_arc_index_prefetch(node, gen->kwg);
  gen->rack[rack_ml]--;
  gen->tiles_left--;
  play_scores_gen_go_on(gen, col, ml, next_node_index, kwg_node_accepts(node),
                        main_word_score, word_multiplier, cross_score);
  gen->tiles_left++;
  gen->rack[rack_ml]++;
}

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
  const uint64_t list_tiles = kwg_get_sibling_tiles(gen->kwg)[node_index];
  for (uint64_t placeable = play_scores_gen_placeable(gen, col, list_tiles);
       placeable != 0; placeable &= placeable - 1) {
    play_scores_gen_place(
        gen, col, node_index, list_tiles,
        (MachineLetter)play_scores_gen_lowest_set_bit(placeable),
        main_word_score, word_multiplier, cross_score);
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

static uint64_t play_scores_build(const Game *game,
                                  const CensusLattice *lattice,
                                  const uint8_t *unseen_tally,
                                  Equity *play_scores, bool shared,
                                  int second_tile_groups,
                                  _Atomic int *next_unit, uint64_t lane_mask,
                                  const uint16_t *anchor_filters) {
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
  gen.shared = shared;
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

  // Every anchor, in the order of a serial build, with the anchor before it
  // in its lane (BOARD_DIM if none), which bounds how far left it extends,
  // and the letters that can be placed on it first. A unit of work is one of
  // these first letters (or, for an anchor on a tile, the anchor) and one of
  // second_tile_groups groups of letters for the tile placed after it, the
  // group of letter ml being ml % second_tile_groups.
  uint8_t row_number_of_anchors[BOARD_DIM * 2];
  board_load_number_of_row_anchors_cache(board, row_number_of_anchors);
  const Square *lanes = board_get_readonly_lanes(board, cross_index);
  const uint32_t root_node_index = kwg_get_root_node_index(gen.kwg);
  const uint64_t root_list_tiles =
      kwg_get_sibling_tiles(gen.kwg)[root_node_index];
  for (int tiles_played = 0; tiles_played < RACK_SIZE; tiles_played++) {
    gen.placeable_masks[tiles_played] = ~(uint64_t)0;
  }
  PlayScoresAnchor anchors[BOARD_DIM * BOARD_DIM * 2];
  int number_of_anchors = 0;
  int number_of_first_tiles = 0;
  for (int lane = 0; lane < BOARD_DIM * 2; lane++) {
    if (row_number_of_anchors[lane] == 0 || ((lane_mask >> lane) & 1) == 0) {
      continue;
    }
    gen.dir = lane / BOARD_DIM;
    gen.row_squares = board_get_row_cache(lanes, lane % BOARD_DIM, gen.dir);
    int last_anchor_col = BOARD_DIM;
    for (int col = 0; col < BOARD_DIM; col++) {
      if (!square_get_anchor(&gen.row_squares[col])) {
        continue;
      }
      if (anchor_filters != NULL &&
          ((anchor_filters[lane] >> col) & 1U) == 0) {
        // Not built, but still the left bound of the next anchor's plays.
        last_anchor_col = col;
        if (!play_scores_gen_is_empty(&gen, col)) {
          last_anchor_col++;
        }
        continue;
      }
      PlayScoresAnchor *anchor = &anchors[number_of_anchors++];
      *anchor =
          (PlayScoresAnchor){.lane = (uint8_t)lane,
                             .col = (uint8_t)col,
                             .last_anchor_col = (uint8_t)last_anchor_col,
                             .on_tile = !play_scores_gen_is_empty(&gen, col),
                             .first_tiles = 0};
      if (!anchor->on_tile) {
        gen.current_anchor_col = col;
        gen.anchor_right_extension_set =
            square_get_right_extension_set(&gen.row_squares[col]);
        if (gen.tiles_left > 0) {
          anchor->first_tiles =
              play_scores_gen_placeable(&gen, col, root_list_tiles);
        }
        number_of_first_tiles +=
            play_scores_gen_count_bits(anchor->first_tiles);
      } else {
        number_of_first_tiles++;
      }
      last_anchor_col = col;
      if (anchor->on_tile) {
        last_anchor_col++;
      }
    }
  }
  // Units are claimed in increasing order, so the anchor they fall in only
  // moves forward.
  int anchor_idx = 0;
  int anchor_first_unit = 0;
  const int number_of_units = number_of_first_tiles * second_tile_groups;
  for (int unit_idx =
           atomic_fetch_add_explicit(next_unit, 1, memory_order_relaxed);
       unit_idx < number_of_units; unit_idx = atomic_fetch_add_explicit(
                                       next_unit, 1, memory_order_relaxed)) {
    const int first_tile_idx = unit_idx / second_tile_groups;
    const int group = unit_idx % second_tile_groups;
    while (true) {
      const PlayScoresAnchor *anchor = &anchors[anchor_idx];
      const int anchor_units =
          anchor->on_tile ? 1 : play_scores_gen_count_bits(anchor->first_tiles);
      if (first_tile_idx < anchor_first_unit + anchor_units) {
        break;
      }
      anchor_first_unit += anchor_units;
      anchor_idx++;
    }
    const PlayScoresAnchor *anchor = &anchors[anchor_idx];
    gen.dir = anchor->lane / BOARD_DIM;
    gen.row_squares =
        board_get_row_cache(lanes, anchor->lane % BOARD_DIM, gen.dir);
    gen.current_anchor_col = anchor->col;
    gen.last_anchor_col = anchor->last_anchor_col;
    gen.anchor_right_extension_set =
        square_get_right_extension_set(&gen.row_squares[anchor->col]);
    gen.tiles_played = 0;
    const uint64_t group_mask =
        play_scores_gen_group_mask(group, second_tile_groups, ld_size);
    if (anchor->on_tile) {
      // The anchor's tile is on the board: the group is that of the first
      // tile placed, and each play of one tile is in one unit.
      gen.placeable_masks[0] = group_mask;
      gen.record_single_tile_plays = true;
      play_scores_gen_recursive(&gen, anchor->col, root_node_index, 0, 1, 0);
      gen.placeable_masks[0] = ~(uint64_t)0;
      continue;
    }
    uint64_t first_tiles = anchor->first_tiles;
    for (int skip_idx = anchor_first_unit; skip_idx < first_tile_idx;
         skip_idx++) {
      first_tiles &= first_tiles - 1;
    }
    gen.placeable_masks[1] = group_mask;
    gen.record_single_tile_plays = group == 0;
    play_scores_gen_place(
        &gen, anchor->col, root_node_index, root_list_tiles,
        (MachineLetter)play_scores_gen_lowest_set_bit(first_tiles), 0, 1, 0);
    gen.placeable_masks[1] = ~(uint64_t)0;
  }
  return gen.number_of_plays;
}

uint64_t census_play_scores_build(const Game *game,
                                  const CensusLattice *lattice,
                                  const uint8_t *unseen_tally,
                                  Equity *play_scores) {
  _Atomic int next_unit;
  atomic_init(&next_unit, 0);
  return play_scores_build(game, lattice, unseen_tally, play_scores, false, 1,
                           &next_unit, CENSUS_ALL_LANES, NULL);
}

uint64_t
census_play_scores_build_claimed(const Game *game, const CensusLattice *lattice,
                                 const uint8_t *unseen_tally,
                                 Equity *play_scores, int second_tile_groups,
                                 _Atomic int *next_unit, uint64_t lane_mask) {
  return play_scores_build(game, lattice, unseen_tally, play_scores, true,
                           second_tile_groups, next_unit, lane_mask, NULL);
}

uint64_t census_play_scores_build_lanes(const Game *game,
                                        const CensusLattice *lattice,
                                        const uint8_t *unseen_tally,
                                        Equity *play_scores,
                                        uint64_t lane_mask) {
  _Atomic int next_unit;
  atomic_init(&next_unit, 0);
  return play_scores_build(game, lattice, unseen_tally, play_scores, false, 1,
                           &next_unit, lane_mask, NULL);
}

uint64_t census_play_scores_build_anchor(const Game *game,
                                         const CensusLattice *lattice,
                                         const uint8_t *unseen_tally,
                                         Equity *play_scores, int lane,
                                         int col) {
  uint16_t anchor_filters[BOARD_DIM * 2] = {0};
  anchor_filters[lane] = (uint16_t)(1U << col);
  _Atomic int next_unit;
  atomic_init(&next_unit, 0);
  return play_scores_build(game, lattice, unseen_tally, play_scores, false, 1,
                           &next_unit, (uint64_t)1 << lane, anchor_filters);
}

uint64_t census_play_scores_changed_lanes(const Game *before,
                                          const Game *after) {
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(before, PLAYERS_DATA_TYPE_KWG), 0);
  const Square *before_lanes =
      board_get_readonly_lanes(game_get_board(before), cross_index);
  const Square *after_lanes =
      board_get_readonly_lanes(game_get_board(after), cross_index);
  uint64_t changed = 0;
  for (int lane = 0; lane < BOARD_DIM * 2; lane++) {
    const Square *before_row =
        board_get_row_cache(before_lanes, lane % BOARD_DIM, lane / BOARD_DIM);
    const Square *after_row =
        board_get_row_cache(after_lanes, lane % BOARD_DIM, lane / BOARD_DIM);
    if (memcmp(before_row, after_row, sizeof(Square) * BOARD_DIM) != 0) {
      changed |= (uint64_t)1 << lane;
    }
  }
  return changed;
}
