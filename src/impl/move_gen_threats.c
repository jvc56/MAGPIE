#include "move_gen_threats.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

enum {
  // The last anchor column before a lane's first anchor: none, so the search
  // from the first anchor runs to the lane's start.
  THREATS_NO_LAST_ANCHOR_COL = BOARD_DIM,
};

// The search of one lane, a row or a column, in the opponent's lexicon. The
// squares are the opponent's (their cross-sets and anchors), indexed along
// the lane.
typedef struct ThreatGen {
  const KWG *kwg;
  const LetterDistribution *ld;
  const Square *lane;
  MoveList *threats;
  Equity bingo_bonus;
  int dir;
  int lane_index;
  int anchor_col;
  int last_anchor_col;
  int tiles_played;
  // How many of each letter the unseen tiles hold, not counting blanks.
  uint16_t naturals[MAX_ALPHABET_SIZE];
  int blanks;
  // The tiles of the opponent's rack that are known, blanks included, and how
  // many more it holds from the rest of the unseen tiles.
  uint16_t known[MAX_ALPHABET_SIZE];
  int unknown_slots;
  // The letters the unseen tiles can play: every letter when they hold a
  // blank.
  uint64_t playable_letters;
  // How many times the play so far places each letter, and how many blanks
  // that takes: one for each placement of a letter beyond its naturals.
  uint16_t placed[MAX_ALPHABET_SIZE];
  int blanks_needed;
  // The play so far, as generate_moves keeps it: an unblanked letter where a
  // tile is placed, PLAYED_THROUGH_MARKER where the lane has one.
  MachineLetter strip[BOARD_DIM];
} ThreatGen;

static Equity threat_tile_score(const ThreatGen *tg, MachineLetter ml) {
  if (get_is_blanked(ml)) {
    return 0;
  }
  return ld_get_score(tg->ld, ml);
}

// What a natural tile of `ml` at `col` adds to the play's score: its score
// times the letter multiplier, in the main word, which the whole play's word
// multiplier multiplies, and in the cross word it makes there, if any, which
// that square's word multiplier multiplies. A blank there adds none of it.
static Equity threat_tile_value(const ThreatGen *tg, int col, MachineLetter ml,
                                int word_multiplier) {
  const BonusSquare bonus = square_get_bonus_square(&tg->lane[col]);
  int multiplier = word_multiplier;
  if (square_get_is_cross_word(&tg->lane[col])) {
    multiplier += bonus_square_get_word_multiplier(bonus);
  }
  return ld_get_score(tg->ld, ml) * bonus_square_get_letter_multiplier(bonus) *
         multiplier;
}

// The square of tiles[leftstrip..rightstrip] whose natural tile adds least to
// the score, among the letters `eligible` marks. False when there is none.
static bool threat_find_cheapest_natural(const ThreatGen *tg,
                                         const MachineLetter *tiles,
                                         int leftstrip, int rightstrip,
                                         const bool *eligible,
                                         int word_multiplier,
                                         int *cheapest_col) {
  bool found = false;
  Equity cheapest_value = EQUITY_MAX_VALUE;
  for (int col = leftstrip; col <= rightstrip; col++) {
    const MachineLetter ml = tiles[col];
    if (ml == PLAYED_THROUGH_MARKER || get_is_blanked(ml) || !eligible[ml]) {
      continue;
    }
    const Equity value = threat_tile_value(tg, col, ml, word_multiplier);
    if (!found || value < cheapest_value) {
      found = true;
      cheapest_value = value;
      *cheapest_col = col;
    }
  }
  return found;
}

// Records the play in strip[leftstrip..rightstrip], if the opponent can hold
// its tiles, as the best-scoring choice of which of them are blanks.
//
// Where it places a letter more times than the unseen tiles have naturals of
// it, the excess are blanks. The opponent's rack is their known tiles and
// unknown_slots more from the rest of the unseen tiles, so the play's tiles
// that are not known must fit in those slots. A known blank the play does not
// already use can stand for a natural that is not known, taking the place of
// an unknown tile. Every blank goes where its letter adds least to the score.
static void threat_record(ThreatGen *tg, int leftstrip, int rightstrip) {
  MachineLetter tiles[BOARD_DIM];
  int word_multiplier = 1;
  uint16_t naturals_used[MAX_ALPHABET_SIZE] = {0};
  for (int col = leftstrip; col <= rightstrip; col++) {
    tiles[col] = tg->strip[col];
    if (tiles[col] != PLAYED_THROUGH_MARKER) {
      word_multiplier *= bonus_square_get_word_multiplier(
          square_get_bonus_square(&tg->lane[col]));
      naturals_used[tiles[col]]++;
    }
  }

  bool eligible[MAX_ALPHABET_SIZE] = {false};
  int blanks_used = 0;
  for (int ml_idx = 1; ml_idx < MAX_ALPHABET_SIZE; ml_idx++) {
    const MachineLetter ml = (MachineLetter)ml_idx;
    if (naturals_used[ml] <= tg->naturals[ml]) {
      continue;
    }
    eligible[ml] = true;
    while (naturals_used[ml] > tg->naturals[ml]) {
      // The play places the letter at least once for each blank it takes.
      int col = leftstrip;
      const bool found = threat_find_cheapest_natural(
          tg, tiles, leftstrip, rightstrip, eligible, word_multiplier, &col);
      assert(found);
      (void)found;
      tiles[col] = get_blanked_machine_letter(ml);
      naturals_used[ml]--;
      blanks_used++;
    }
    eligible[ml] = false;
  }

  int unknown_tiles = 0;
  for (int ml_idx = 1; ml_idx < MAX_ALPHABET_SIZE; ml_idx++) {
    if (naturals_used[ml_idx] > tg->known[ml_idx]) {
      unknown_tiles += naturals_used[ml_idx] - tg->known[ml_idx];
      eligible[ml_idx] = true;
    }
  }
  if (blanks_used > tg->known[BLANK_MACHINE_LETTER]) {
    unknown_tiles += blanks_used - tg->known[BLANK_MACHINE_LETTER];
  }
  while (unknown_tiles > tg->unknown_slots &&
         blanks_used < tg->known[BLANK_MACHINE_LETTER]) {
    int col = leftstrip;
    if (!threat_find_cheapest_natural(tg, tiles, leftstrip, rightstrip,
                                      eligible, word_multiplier, &col)) {
      break;
    }
    const MachineLetter ml = tiles[col];
    tiles[col] = get_blanked_machine_letter(ml);
    naturals_used[ml]--;
    eligible[ml] = naturals_used[ml] > tg->known[ml];
    blanks_used++;
    unknown_tiles--;
  }
  if (unknown_tiles > tg->unknown_slots) {
    return;
  }

  Equity main_word_score = 0;
  Equity cross_scores = 0;
  for (int col = leftstrip; col <= rightstrip; col++) {
    const Square *square = &tg->lane[col];
    if (tiles[col] == PLAYED_THROUGH_MARKER) {
      main_word_score += threat_tile_score(tg, square_get_letter(square));
      continue;
    }
    const BonusSquare bonus = square_get_bonus_square(square);
    const Equity letter_score = threat_tile_score(tg, tiles[col]) *
                                bonus_square_get_letter_multiplier(bonus);
    main_word_score += letter_score;
    if (square_get_is_cross_word(square)) {
      cross_scores += (letter_score + square_get_cross_score(square)) *
                      bonus_square_get_word_multiplier(bonus);
    }
  }
  Equity score = main_word_score * word_multiplier + cross_scores;
  if (tg->tiles_played == RACK_SIZE) {
    score += tg->bingo_bonus;
  }

  Move *move = move_list_get_spare_move(tg->threats);
  move_set_all_except_equity(move, tiles, leftstrip, rightstrip, score,
                             tg->lane_index, leftstrip, tg->tiles_played,
                             tg->dir, GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(tg->dir)) {
    move_set_row_start(move, leftstrip);
    move_set_col_start(move, tg->lane_index);
  }
  move_list_insert_spare_move(tg->threats, score);
}

static void threat_go_on(ThreatGen *tg, int col, MachineLetter ml,
                         uint32_t new_node_index, bool accepts, int leftstrip,
                         int rightstrip, bool unique_play);

// As recursive_gen in move_gen.c: extends the play from the anchor through
// `col`, from the lexicon's node_index.
static void threat_recursive_gen(ThreatGen *tg, int col, uint32_t node_index,
                                 int leftstrip, int rightstrip,
                                 bool unique_play) {
  const Square *square = &tg->lane[col];
  const MachineLetter board_letter = square_get_letter(square);
  const uint64_t list_tiles = kwg_get_sibling_tiles(tg->kwg)[node_index];
  if (board_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const uint64_t tile_bit = (uint64_t)1
                              << get_unblanked_machine_letter(board_letter);
    uint32_t next_node_index = 0;
    bool accepts = false;
    if ((list_tiles & tile_bit) != 0) {
      const uint32_t node =
          kwg_list_node(tg->kwg, node_index, list_tiles, tile_bit);
      next_node_index = kwg_node_arc_index(node);
      accepts = kwg_node_accepts(node);
    }
    threat_go_on(tg, col, board_letter, next_node_index, accepts, leftstrip,
                 rightstrip, unique_play);
    return;
  }
  if (tg->tiles_played == RACK_SIZE) {
    return;
  }
  uint64_t placeable = square_get_cross_set(square) & list_tiles &
                       tg->playable_letters & ~(uint64_t)1;
  for (; placeable != 0; placeable &= placeable - 1) {
    const MachineLetter ml =
        (MachineLetter)kwg_bit_count_64((placeable & -placeable) - 1);
    const bool takes_blank = tg->placed[ml] >= tg->naturals[ml];
    if (takes_blank && tg->blanks_needed == tg->blanks) {
      continue;
    }
    const uint32_t node =
        kwg_list_node(tg->kwg, node_index, list_tiles, (uint64_t)1 << ml);
    tg->placed[ml]++;
    tg->blanks_needed += takes_blank;
    tg->tiles_played++;
    threat_go_on(tg, col, ml, kwg_node_arc_index(node), kwg_node_accepts(node),
                 leftstrip, rightstrip, unique_play);
    tg->tiles_played--;
    tg->blanks_needed -= takes_blank;
    tg->placed[ml]--;
  }
}

// As go_on in move_gen.c: places `ml` at `col` (or plays through the lane's
// tile there), records the play if it makes a word, and goes on left, or
// right from the anchor.
static void threat_go_on(ThreatGen *tg, int col, MachineLetter ml,
                         uint32_t new_node_index, bool accepts, int leftstrip,
                         int rightstrip, bool unique_play) {
  const Square *square = &tg->lane[col];
  const bool square_is_empty =
      square_get_letter(square) == ALPHABET_EMPTY_SQUARE_MARKER;
  tg->strip[col] = square_is_empty ? ml : PLAYED_THROUGH_MARKER;
  // A one-tile play is found in both directions. As in generate_moves, it is
  // recorded across, or down when it makes no word across.
  if (square_is_empty && board_is_dir_vertical(tg->dir) &&
      square_get_cross_set(square) == TRIVIAL_CROSS_SET) {
    unique_play = true;
  }
  const bool records = accepts && (tg->tiles_played > 1 ||
                                   (tg->tiles_played == 1 && unique_play));

  if (col <= tg->anchor_col) {
    leftstrip = col;
    const bool no_letter_directly_left =
        col == 0 ||
        square_get_letter(&tg->lane[col - 1]) == ALPHABET_EMPTY_SQUARE_MARKER;
    if (records && no_letter_directly_left) {
      threat_record(tg, leftstrip, rightstrip);
    }
    if (new_node_index == 0) {
      return;
    }
    if (col > 0 && col - 1 != tg->last_anchor_col) {
      threat_recursive_gen(tg, col - 1, new_node_index, leftstrip, rightstrip,
                           unique_play);
    }
    const uint32_t separation_node_index = kwg_get_next_node_index(
        tg->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        tg->anchor_col < BOARD_DIM - 1) {
      threat_recursive_gen(tg, tg->anchor_col + 1, separation_node_index,
                           leftstrip, rightstrip, unique_play);
    }
    return;
  }
  rightstrip = col;
  const bool no_letter_directly_right =
      col == BOARD_DIM - 1 ||
      square_get_letter(&tg->lane[col + 1]) == ALPHABET_EMPTY_SQUARE_MARKER;
  if (records && no_letter_directly_right) {
    threat_record(tg, leftstrip, rightstrip);
  }
  if (new_node_index != 0 && col < BOARD_DIM - 1) {
    threat_recursive_gen(tg, col + 1, new_node_index, leftstrip, rightstrip,
                         unique_play);
  }
}

void threats_get_unseen_tiles(const Game *game, Rack *unseen) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  rack_set_dist_size_and_reset(unseen, ld_size);
  const int opponent_index = 1 - game_get_player_on_turn_index(game);
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, opponent_index));
  const Bag *bag = game_get_bag(game);
  for (int ml_idx = 0; ml_idx < ld_size; ml_idx++) {
    const MachineLetter ml = (MachineLetter)ml_idx;
    const int count =
        rack_get_letter(opponent_rack, ml) + bag_get_letter(bag, ml);
    for (int count_idx = 0; count_idx < count; count_idx++) {
      rack_add_letter(unseen, ml);
    }
  }
}

void generate_threats(const Game *game, const Rack *unseen, const Rack *known,
                      MoveList *threats) {
  move_list_reset(threats);
  const Board *board = game_get_board(game);
  assert(board_get_cross_sets_valid(board));
  const int opponent_index = 1 - game_get_player_on_turn_index(game);
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), opponent_index);

  ThreatGen tg = {
      .kwg = player_get_kwg(game_get_player(game, opponent_index)),
      .ld = game_get_ld(game),
      .threats = threats,
      .bingo_bonus = game_get_bingo_bonus(game),
      .blanks = rack_get_letter(unseen, BLANK_MACHINE_LETTER),
  };
  for (int ml_idx = 1; ml_idx < rack_get_dist_size(unseen); ml_idx++) {
    const MachineLetter ml = (MachineLetter)ml_idx;
    tg.naturals[ml] = rack_get_letter(unseen, ml);
    if (tg.naturals[ml] > 0 || tg.blanks > 0) {
      tg.playable_letters |= (uint64_t)1 << ml;
    }
  }
  // The opponent holds a full rack, or every unseen tile when fewer are left.
  int opponent_rack_size = rack_get_total_letters(unseen);
  if (opponent_rack_size > RACK_SIZE) {
    opponent_rack_size = RACK_SIZE;
  }
  tg.unknown_slots = opponent_rack_size;
  for (int ml_idx = 0; ml_idx < rack_get_dist_size(known); ml_idx++) {
    const MachineLetter ml = (MachineLetter)ml_idx;
    assert(rack_get_letter(known, ml) <= rack_get_letter(unseen, ml));
    tg.known[ml] = rack_get_letter(known, ml);
    tg.unknown_slots -= tg.known[ml];
  }
  assert(tg.unknown_slots >= 0);
  if (tg.playable_letters != 0) {
    const Square *lanes = board_get_readonly_lanes(board, cross_index);
    const uint32_t root_node_index = kwg_get_root_node_index(tg.kwg);
    for (int dir = 0; dir < 2; dir++) {
      tg.dir = dir;
      for (int lane_index = 0; lane_index < BOARD_DIM; lane_index++) {
        tg.lane_index = lane_index;
        tg.lane = board_get_row_cache(lanes, lane_index, dir);
        tg.last_anchor_col = THREATS_NO_LAST_ANCHOR_COL;
        for (int col = 0; col < BOARD_DIM; col++) {
          if (!square_get_anchor(&tg.lane[col])) {
            continue;
          }
          tg.anchor_col = col;
          threat_recursive_gen(&tg, col, root_node_index, col, col,
                               !board_is_dir_vertical(dir));
          tg.last_anchor_col = col;
          // As in generate_moves: the next anchor's search stops a square
          // short of a tile here, which this anchor's search played through.
          if (square_get_letter(&tg.lane[col]) !=
              ALPHABET_EMPTY_SQUARE_MARKER) {
            tg.last_anchor_col++;
          }
        }
      }
    }
  }
  move_list_sort_moves(threats);
}
