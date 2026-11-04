#include "move_gen_threats.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "census_play_scores.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// What a play must fit to be a threat: the unseen tiles, and the opponent's
// rack, which is their known tiles and unknown_slots more from the rest of the
// unseen tiles.
typedef struct ThreatRecorder {
  MoveList *threats;
  // How many of each letter the unseen tiles hold, not counting blanks.
  uint16_t naturals[MAX_ALPHABET_SIZE];
  // The known tiles, blanks included.
  uint16_t known[MAX_ALPHABET_SIZE];
  int unknown_slots;
} ThreatRecorder;

// The placed tile that loses least as a blank, among the tiles not yet blanks
// whose letters `eligible` marks (by the play's letter index, letter_of_tile),
// the leftmost of equals. False when there is none.
static bool threat_find_cheapest_natural(const CensusPlay *play,
                                         const int *letter_of_tile,
                                         const bool *is_blank,
                                         const bool *eligible,
                                         int *cheapest_tile_idx) {
  bool found = false;
  Equity cheapest_drop = EQUITY_MAX_VALUE;
  int cheapest_col = BOARD_DIM;
  for (int tile_idx = 0; tile_idx < play->tiles_played; tile_idx++) {
    if (is_blank[tile_idx] || !eligible[letter_of_tile[tile_idx]]) {
      continue;
    }
    const Equity drop = census_play_blank_drop(play, tile_idx);
    const int col = play->tiles[tile_idx].col;
    if (!found || drop < cheapest_drop ||
        (drop == cheapest_drop && col < cheapest_col)) {
      found = true;
      cheapest_drop = drop;
      cheapest_col = col;
      *cheapest_tile_idx = tile_idx;
    }
  }
  return found;
}

// Records the play, if the opponent can hold its tiles, as the best-scoring
// choice of which of them are blanks.
//
// Where it places a letter more times than the unseen tiles have naturals of
// it, the excess are blanks. The play's tiles that are not known must fit in
// the opponent's unknown slots. A known blank the play does not already use
// can stand for a natural that is not known, taking the place of an unknown
// tile. Every blank goes where its letter adds least to the score.
static void threat_record(const CensusPlay *play, void *context) {
  const ThreatRecorder *recorder = context;
  // A one-tile play is found in both directions. As in generate_moves, it is
  // recorded across, or down when it makes no word across.
  if (play->tiles_played == 1 && board_is_dir_vertical(play->dir) &&
      square_get_cross_set(&play->lane[play->tiles[0].col]) !=
          TRIVIAL_CROSS_SET) {
    return;
  }
  // The play's score counts every tile at face value, and blanks only take
  // value off, so once the list is full a play scoring less than its worst
  // threat cannot join it.
  const MoveList *threats = recorder->threats;
  if (move_list_get_count(threats) == move_list_get_capacity(threats) &&
      play->score < move_get_equity(move_list_get_move(threats, 0))) {
    return;
  }

  // The play's distinct letters, the naturals it places of each, and each
  // tile's letter among them.
  MachineLetter letters[RACK_SIZE];
  int naturals_used[RACK_SIZE] = {0};
  int letter_of_tile[RACK_SIZE];
  int number_of_letters = 0;
  for (int tile_idx = 0; tile_idx < play->tiles_played; tile_idx++) {
    const MachineLetter ml = play->tiles[tile_idx].ml;
    int letter_idx = 0;
    while (letter_idx < number_of_letters && letters[letter_idx] != ml) {
      letter_idx++;
    }
    if (letter_idx == number_of_letters) {
      letters[number_of_letters++] = ml;
    }
    letter_of_tile[tile_idx] = letter_idx;
    naturals_used[letter_idx]++;
  }

  bool is_blank[RACK_SIZE] = {false};
  bool eligible[RACK_SIZE] = {false};
  Equity score = play->score;
  int blanks_used = 0;
  for (int letter_idx = 0; letter_idx < number_of_letters; letter_idx++) {
    const int naturals = recorder->naturals[letters[letter_idx]];
    if (naturals_used[letter_idx] <= naturals) {
      continue;
    }
    eligible[letter_idx] = true;
    while (naturals_used[letter_idx] > naturals) {
      // The play places the letter at least once for each blank it takes.
      int tile_idx = 0;
      const bool found = threat_find_cheapest_natural(
          play, letter_of_tile, is_blank, eligible, &tile_idx);
      assert(found);
      (void)found;
      is_blank[tile_idx] = true;
      score -= census_play_blank_drop(play, tile_idx);
      naturals_used[letter_idx]--;
      blanks_used++;
    }
    eligible[letter_idx] = false;
  }

  int unknown_tiles = 0;
  for (int letter_idx = 0; letter_idx < number_of_letters; letter_idx++) {
    const int known = recorder->known[letters[letter_idx]];
    if (naturals_used[letter_idx] > known) {
      unknown_tiles += naturals_used[letter_idx] - known;
      eligible[letter_idx] = true;
    }
  }
  const int known_blanks = recorder->known[BLANK_MACHINE_LETTER];
  if (blanks_used > known_blanks) {
    unknown_tiles += blanks_used - known_blanks;
  }
  while (unknown_tiles > recorder->unknown_slots &&
         blanks_used < known_blanks) {
    int tile_idx = 0;
    if (!threat_find_cheapest_natural(play, letter_of_tile, is_blank, eligible,
                                      &tile_idx)) {
      break;
    }
    const int letter_idx = letter_of_tile[tile_idx];
    is_blank[tile_idx] = true;
    score -= census_play_blank_drop(play, tile_idx);
    naturals_used[letter_idx]--;
    eligible[letter_idx] =
        naturals_used[letter_idx] > recorder->known[letters[letter_idx]];
    blanks_used++;
    unknown_tiles--;
  }
  if (unknown_tiles > recorder->unknown_slots) {
    return;
  }
  MachineLetter tiles[BOARD_DIM];
  for (int col = play->leftstrip; col <= play->rightstrip; col++) {
    tiles[col] = PLAYED_THROUGH_MARKER;
  }
  for (int tile_idx = 0; tile_idx < play->tiles_played; tile_idx++) {
    const CensusPlayTile *tile = &play->tiles[tile_idx];
    tiles[tile->col] =
        is_blank[tile_idx] ? get_blanked_machine_letter(tile->ml) : tile->ml;
  }
  Move *move = move_list_get_spare_move(recorder->threats);
  move_set_all_except_equity(move, tiles, play->leftstrip, play->rightstrip,
                             score, play->lane_index, play->leftstrip,
                             play->tiles_played, play->dir,
                             GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(play->dir)) {
    move_set_row_start(move, play->leftstrip);
    move_set_col_start(move, play->lane_index);
  }
  move_list_insert_spare_move(recorder->threats, score);
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
  assert(board_get_cross_sets_valid(game_get_board(game)));
  const int opponent_index = 1 - game_get_player_on_turn_index(game);

  ThreatRecorder recorder = {.threats = threats};
  uint8_t unseen_tally[MAX_ALPHABET_SIZE] = {0};
  for (int ml_idx = 0; ml_idx < rack_get_dist_size(unseen); ml_idx++) {
    const MachineLetter ml = (MachineLetter)ml_idx;
    unseen_tally[ml] = (uint8_t)rack_get_letter(unseen, ml);
    if (ml != BLANK_MACHINE_LETTER) {
      recorder.naturals[ml] = unseen_tally[ml];
    }
  }
  // The opponent holds a full rack, or every unseen tile when fewer are left.
  int opponent_rack_size = rack_get_total_letters(unseen);
  if (opponent_rack_size > RACK_SIZE) {
    opponent_rack_size = RACK_SIZE;
  }
  recorder.unknown_slots = opponent_rack_size;
  for (int ml_idx = 0; ml_idx < rack_get_dist_size(known); ml_idx++) {
    const MachineLetter ml = (MachineLetter)ml_idx;
    assert(rack_get_letter(known, ml) <= rack_get_letter(unseen, ml));
    recorder.known[ml] = rack_get_letter(known, ml);
    recorder.unknown_slots -= recorder.known[ml];
  }
  assert(recorder.unknown_slots >= 0);
  census_play_scores_search(game, opponent_index, unseen_tally, threat_record,
                            &recorder);
  move_list_sort_moves(threats);
}
