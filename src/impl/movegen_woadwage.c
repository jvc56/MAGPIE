#include "movegen_woadwage.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/kwg.h"
#include "../ent/leave_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "move_gen_base.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// A woadwage word is one word of the lexicon or two run together. The search
// goes left from the anchor and then right, as recursive_gen does, but follows
// a set of states instead of one GADDAG node: one for each way the letters
// read so far can begin a woadwage word. Every state in a set has read the
// same letters, so each play is still reached once, and is recorded once if
// any of them accepts it.
//
// Going left, the anchor's word is read from the GADDAG root. Wherever it can
// begin, on the square just read, a state starts reading a first word that
// ends on the square to its left, its letters backward from the GADDAG root,
// keeping where the anchor's word continues right. Going right, the anchor's
// word continues past the GADDAG's separator, and wherever it can end, unless
// a first word was read on the left, a state starts reading a second word
// forward from the GADDAG root: its first letter, the separator, then the
// rest. Only the GADDAG is read, since the endgame's pruned KWGs have no DAWG.

enum {
  // The most states a set holds: on the left, the anchor's word and a first
  // word begun at each square; on the right, the anchor's word continued from
  // each of those, and a second word begun at each square.
  WOADWAGE_MAX_STATES = 2 * BOARD_DIM + 2,
};

typedef enum {
  // The anchor's word, read left from the anchor in the GADDAG.
  WOADWAGE_LEFT_ANCHOR_WORD,
  // A first word left of the anchor's word, read backward in the GADDAG.
  WOADWAGE_LEFT_FIRST_WORD,
} woadwage_left_t;

typedef enum {
  // The anchor's word, read right of the anchor after the GADDAG's separator.
  WOADWAGE_RIGHT_ANCHOR_WORD,
  // A second word right of the anchor's word, read forward in the GADDAG.
  WOADWAGE_RIGHT_SECOND_WORD,
} woadwage_right_t;

typedef struct WoadwageLeftState {
  woadwage_left_t type;
  // The list the next letter to the left is read from; 0 if none can be.
  uint32_t node_index;
  // Whether the letters this state has read end its word.
  bool accepts;
  // For a first word: the list the anchor's word continues from right of the
  // anchor (0 if it cannot), and whether it is already a word.
  uint32_t anchor_word_separator_node_index;
  bool anchor_word_accepts;
} WoadwageLeftState;

typedef struct WoadwageRightState {
  woadwage_right_t type;
  // The list the next letter to the right is read from; 0 if none can be.
  uint32_t node_index;
  // Whether the letters this state has read end its word.
  bool accepts;
  // For the anchor's word: whether no first word was read on the left, so a
  // second word may follow it.
  bool may_add_second_word;
  // For a second word: whether it has read its first letter. Until it has,
  // node_index is the GADDAG root, and the separator follows the letter.
  bool read_first_letter;
} WoadwageRightState;

typedef struct WoadwageLeftStates {
  int count;
  WoadwageLeftState states[WOADWAGE_MAX_STATES];
} WoadwageLeftStates;

typedef struct WoadwageRightStates {
  int count;
  WoadwageRightState states[WOADWAGE_MAX_STATES];
} WoadwageRightStates;

// The arc for ml out of the list at node_index: whether the list has one, and
// if it does, the list it leads to and whether it ends a word.
static inline bool woadwage_follow(const KWG *kwg, uint32_t node_index,
                                   MachineLetter ml, uint32_t *next_node_index,
                                   bool *accepts) {
  if (node_index == 0) {
    return false;
  }
  const uint64_t list_tiles = kwg_get_sibling_tiles(kwg)[node_index];
  const uint64_t tile_bit = (uint64_t)1 << ml;
  if ((list_tiles & tile_bit) == 0) {
    return false;
  }
  const uint32_t node = kwg_list_node(kwg, node_index, list_tiles, tile_bit);
  *next_node_index = kwg_node_arc_index_prefetch(node, kwg);
  *accepts = kwg_node_accepts(node);
  return true;
}

static inline uint64_t woadwage_list_tiles(const KWG *kwg,
                                           uint32_t node_index) {
  return node_index == 0 ? 0 : kwg_get_sibling_tiles(kwg)[node_index];
}

static void woadwage_add_left_state(WoadwageLeftStates *states,
                                    const WoadwageLeftState *state) {
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    const WoadwageLeftState *other = &states->states[state_idx];
    if (other->type == state->type && other->node_index == state->node_index &&
        other->accepts == state->accepts &&
        other->anchor_word_separator_node_index ==
            state->anchor_word_separator_node_index &&
        other->anchor_word_accepts == state->anchor_word_accepts) {
      return;
    }
  }
  assert(states->count < WOADWAGE_MAX_STATES);
  states->states[states->count++] = *state;
}

static void woadwage_add_right_state(WoadwageRightStates *states,
                                     const WoadwageRightState *state) {
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    const WoadwageRightState *other = &states->states[state_idx];
    if (other->type == state->type && other->node_index == state->node_index &&
        other->accepts == state->accepts &&
        other->may_add_second_word == state->may_add_second_word &&
        other->read_first_letter == state->read_first_letter) {
      return;
    }
  }
  assert(states->count < WOADWAGE_MAX_STATES);
  states->states[states->count++] = *state;
}

// The letters some state of the set can read next.
static uint64_t woadwage_left_next_letters(const KWG *kwg,
                                           const WoadwageLeftStates *states) {
  uint64_t letters = 0;
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    letters |= woadwage_list_tiles(kwg, states->states[state_idx].node_index);
  }
  return letters & ~(uint64_t)1;
}

static uint64_t woadwage_right_next_letters(const KWG *kwg,
                                            const WoadwageRightStates *states) {
  uint64_t letters = 0;
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    letters |= woadwage_list_tiles(kwg, states->states[state_idx].node_index);
  }
  return letters & ~(uint64_t)1;
}

// Reads ml, on the square left of the letters read so far, into each state
// that can read it.
static void woadwage_read_left(const KWG *kwg, const WoadwageLeftStates *states,
                               MachineLetter ml, WoadwageLeftStates *read) {
  read->count = 0;
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    WoadwageLeftState state = states->states[state_idx];
    if (woadwage_follow(kwg, state.node_index, ml, &state.node_index,
                        &state.accepts)) {
      woadwage_add_left_state(read, &state);
    }
  }
}

static void woadwage_read_right(const KWG *kwg,
                                const WoadwageRightStates *states,
                                MachineLetter ml, WoadwageRightStates *read) {
  read->count = 0;
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    WoadwageRightState state = states->states[state_idx];
    if (!woadwage_follow(kwg, state.node_index, ml, &state.node_index,
                         &state.accepts)) {
      continue;
    }
    if (!state.read_first_letter) {
      state.read_first_letter = true;
      state.node_index =
          state.node_index == 0
              ? 0
              : kwg_get_next_node_index(kwg, state.node_index,
                                        SEPARATION_MACHINE_LETTER);
    }
    woadwage_add_right_state(read, &state);
  }
}

// Whether the letters read, from the square just read to the anchor, are a
// woadwage word: the anchor's word alone, or a first word and then the
// anchor's word.
static bool woadwage_left_accepts(const WoadwageLeftStates *states) {
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    const WoadwageLeftState *state = &states->states[state_idx];
    if (state->accepts && (state->type == WOADWAGE_LEFT_ANCHOR_WORD ||
                           state->anchor_word_accepts)) {
      return true;
    }
  }
  return false;
}

static bool woadwage_right_accepts(const WoadwageRightStates *states) {
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    if (states->states[state_idx].accepts) {
      return true;
    }
  }
  return false;
}

// The states that read on to the left of the square just read: those that
// still can, and a first word ending on the square to the left if the anchor's
// word can begin on this one.
static void woadwage_continue_left(const KWG *kwg,
                                   const WoadwageLeftStates *states,
                                   WoadwageLeftStates *continuing) {
  continuing->count = 0;
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    const WoadwageLeftState *state = &states->states[state_idx];
    if (state->node_index != 0) {
      woadwage_add_left_state(continuing, state);
    }
    if (state->type != WOADWAGE_LEFT_ANCHOR_WORD) {
      continue;
    }
    const uint32_t separator_node_index =
        state->node_index == 0
            ? 0
            : kwg_get_next_node_index(kwg, state->node_index,
                                      SEPARATION_MACHINE_LETTER);
    if (separator_node_index != 0 || state->accepts) {
      const WoadwageLeftState first_word = {
          .type = WOADWAGE_LEFT_FIRST_WORD,
          .node_index = kwg_get_root_node_index(kwg),
          .accepts = false,
          .anchor_word_separator_node_index = separator_node_index,
          .anchor_word_accepts = state->accepts,
      };
      woadwage_add_left_state(continuing, &first_word);
    }
  }
}

// The states that read right of the anchor once the left part ends on the
// square just read: each way the anchor's word continues, and a second word
// after it if it is already a word and no first word was read.
static void woadwage_start_right(const KWG *kwg,
                                 const WoadwageLeftStates *states,
                                 WoadwageRightStates *right) {
  right->count = 0;
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    const WoadwageLeftState *state = &states->states[state_idx];
    if (state->type == WOADWAGE_LEFT_ANCHOR_WORD) {
      if (state->node_index != 0) {
        const uint32_t separator_node_index = kwg_get_next_node_index(
            kwg, state->node_index, SEPARATION_MACHINE_LETTER);
        if (separator_node_index != 0) {
          const WoadwageRightState anchor_word = {
              .type = WOADWAGE_RIGHT_ANCHOR_WORD,
              .node_index = separator_node_index,
              .accepts = false,
              .may_add_second_word = true,
              .read_first_letter = true,
          };
          woadwage_add_right_state(right, &anchor_word);
        }
      }
      if (state->accepts) {
        const WoadwageRightState second_word = {
            .type = WOADWAGE_RIGHT_SECOND_WORD,
            .node_index = kwg_get_root_node_index(kwg),
            .accepts = false,
            .may_add_second_word = false,
            .read_first_letter = false,
        };
        woadwage_add_right_state(right, &second_word);
      }
    } else if (state->accepts && state->anchor_word_separator_node_index != 0) {
      const WoadwageRightState anchor_word = {
          .type = WOADWAGE_RIGHT_ANCHOR_WORD,
          .node_index = state->anchor_word_separator_node_index,
          .accepts = false,
          .may_add_second_word = false,
          .read_first_letter = true,
      };
      woadwage_add_right_state(right, &anchor_word);
    }
  }
}

// The states that read on to the right of the square just read: those that
// still can, and a second word if the anchor's word can end on this square.
static void woadwage_continue_right(const KWG *kwg,
                                    const WoadwageRightStates *states,
                                    WoadwageRightStates *continuing) {
  continuing->count = 0;
  for (int state_idx = 0; state_idx < states->count; state_idx++) {
    const WoadwageRightState *state = &states->states[state_idx];
    if (state->node_index != 0) {
      woadwage_add_right_state(continuing, state);
    }
    if (state->type == WOADWAGE_RIGHT_ANCHOR_WORD && state->accepts &&
        state->may_add_second_word) {
      const WoadwageRightState second_word = {
          .type = WOADWAGE_RIGHT_SECOND_WORD,
          .node_index = kwg_get_root_node_index(kwg),
          .accepts = false,
          .may_add_second_word = false,
          .read_first_letter = false,
      };
      woadwage_add_right_state(continuing, &second_word);
    }
  }
}

// The record types for the endgame take tiles from the rack without the leave
// map, which they do not set up.
static inline bool woadwage_records_leaves(const MoveGen *gen) {
  return gen->move_record_type != MOVE_RECORD_ALL_SMALL &&
         gen->move_record_type != MOVE_RECORD_TILES_PLAYED &&
         gen->move_record_type != MOVE_RECORD_BEST_SMALL;
}

static inline void woadwage_take_tile(MoveGen *gen, MachineLetter ml) {
  if (woadwage_records_leaves(gen)) {
    leave_map_take_letter_and_update_current_index(&gen->leave_map,
                                                   &gen->player_rack, ml);
  } else {
    rack_take_letter(&gen->player_rack, ml);
  }
  gen->tiles_played++;
}

static inline void woadwage_return_tile(MoveGen *gen, MachineLetter ml) {
  gen->tiles_played--;
  if (woadwage_records_leaves(gen)) {
    leave_map_add_letter_and_update_current_index(&gen->leave_map,
                                                  &gen->player_rack, ml);
  } else {
    rack_add_letter(&gen->player_rack, ml);
  }
}

// The scores so far with ml, the letter on col, added: the main word's tile
// score and word multiplier, and the cross words' scores.
static inline void woadwage_add_square_score(const MoveGen *gen, int col,
                                             MachineLetter ml,
                                             Equity *main_word_score,
                                             int *word_multiplier,
                                             Equity *cross_score) {
  if (!gen_cache_is_empty(gen, col)) {
    *main_word_score += gen->tile_scores[gen_cache_get_letter(gen, col)];
    return;
  }
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, col);
  const int this_word_multiplier =
      bonus_square_get_word_multiplier(bonus_square);
  const Equity letter_score =
      gen->tile_scores[ml] * bonus_square_get_letter_multiplier(bonus_square);
  *main_word_score += letter_score;
  *word_multiplier *= this_word_multiplier;
  if (gen_cache_get_is_cross_word(gen, col)) {
    *cross_score += (letter_score + gen_cache_get_cross_score(gen, col)) *
                    this_word_multiplier;
  }
}

static void woadwage_go_on_left(MoveGen *gen, int col, MachineLetter ml,
                                const WoadwageLeftStates *states,
                                int rightstrip, bool unique_play,
                                Equity main_word_score, int word_multiplier,
                                Equity cross_score);

static void woadwage_go_on_right(MoveGen *gen, int col, MachineLetter ml,
                                 const WoadwageRightStates *states,
                                 int leftstrip, bool unique_play,
                                 Equity main_word_score, int word_multiplier,
                                 Equity cross_score);

// Places or reads the letter on col, left of the anchor or on it, as
// recursive_gen does.
static void woadwage_gen_left(MoveGen *gen, int col,
                              const WoadwageLeftStates *states, int rightstrip,
                              bool unique_play, Equity main_word_score,
                              int word_multiplier, Equity cross_score) {
  if (gen->threshold_exceeded) {
    return;
  }
  WoadwageLeftStates read;
  const MachineLetter current_letter = gen_cache_get_letter(gen, col);
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    woadwage_read_left(gen->kwg, states,
                       get_unblanked_machine_letter(current_letter), &read);
    if (read.count > 0) {
      woadwage_go_on_left(gen, col, current_letter, &read, rightstrip,
                          unique_play, main_word_score, word_multiplier,
                          cross_score);
    }
    return;
  }
  if (rack_is_empty(&gen->player_rack)) {
    return;
  }
  const bool has_blank =
      rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) > 0;
  uint64_t placeable = gen_cache_get_cross_set(gen, col) &
                       woadwage_left_next_letters(gen->kwg, states);
  if (!has_blank) {
    placeable &= gen->rack_cross_set;
  }
  for (; placeable != 0; placeable &= placeable - 1) {
    const MachineLetter ml = (MachineLetter)lowest_set_bit_index_64(placeable);
    const uint16_t number_of_ml = rack_get_letter(&gen->player_rack, ml);
    if (number_of_ml == 0 && !has_blank) {
      continue;
    }
    woadwage_read_left(gen->kwg, states, ml, &read);
    if (number_of_ml > 0) {
      woadwage_take_tile(gen, ml);
      woadwage_go_on_left(gen, col, ml, &read, rightstrip, unique_play,
                          main_word_score, word_multiplier, cross_score);
      woadwage_return_tile(gen, ml);
    }
    if (has_blank) {
      woadwage_take_tile(gen, BLANK_MACHINE_LETTER);
      woadwage_go_on_left(gen, col, get_blanked_machine_letter(ml), &read,
                          rightstrip, unique_play, main_word_score,
                          word_multiplier, cross_score);
      woadwage_return_tile(gen, BLANK_MACHINE_LETTER);
    }
    if (gen->threshold_exceeded) {
      break;
    }
  }
}

// Places or reads the letter on col, right of the anchor.
static void woadwage_gen_right(MoveGen *gen, int col,
                               const WoadwageRightStates *states, int leftstrip,
                               bool unique_play, Equity main_word_score,
                               int word_multiplier, Equity cross_score) {
  if (gen->threshold_exceeded) {
    return;
  }
  WoadwageRightStates read;
  const MachineLetter current_letter = gen_cache_get_letter(gen, col);
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    woadwage_read_right(gen->kwg, states,
                        get_unblanked_machine_letter(current_letter), &read);
    if (read.count > 0) {
      woadwage_go_on_right(gen, col, current_letter, &read, leftstrip,
                           unique_play, main_word_score, word_multiplier,
                           cross_score);
    }
    return;
  }
  if (rack_is_empty(&gen->player_rack)) {
    return;
  }
  const bool has_blank =
      rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) > 0;
  uint64_t placeable = gen_cache_get_cross_set(gen, col) &
                       woadwage_right_next_letters(gen->kwg, states);
  if (!has_blank) {
    placeable &= gen->rack_cross_set;
  }
  for (; placeable != 0; placeable &= placeable - 1) {
    const MachineLetter ml = (MachineLetter)lowest_set_bit_index_64(placeable);
    const uint16_t number_of_ml = rack_get_letter(&gen->player_rack, ml);
    if (number_of_ml == 0 && !has_blank) {
      continue;
    }
    woadwage_read_right(gen->kwg, states, ml, &read);
    if (number_of_ml > 0) {
      woadwage_take_tile(gen, ml);
      woadwage_go_on_right(gen, col, ml, &read, leftstrip, unique_play,
                           main_word_score, word_multiplier, cross_score);
      woadwage_return_tile(gen, ml);
    }
    if (has_blank) {
      woadwage_take_tile(gen, BLANK_MACHINE_LETTER);
      woadwage_go_on_right(gen, col, get_blanked_machine_letter(ml), &read,
                           leftstrip, unique_play, main_word_score,
                           word_multiplier, cross_score);
      woadwage_return_tile(gen, BLANK_MACHINE_LETTER);
    }
    if (gen->threshold_exceeded) {
      break;
    }
  }
}

// With the letter ml on col, left of the anchor or on it, and read into
// states: records the play if it ends here, then reads on to the left and
// starts the part right of the anchor, as go_on does.
static void woadwage_go_on_left(MoveGen *gen, int col, MachineLetter ml,
                                const WoadwageLeftStates *states,
                                int rightstrip, bool unique_play,
                                Equity main_word_score, int word_multiplier,
                                Equity cross_score) {
  if (gen->threshold_exceeded) {
    return;
  }
  const bool square_is_empty = gen_cache_is_empty(gen, col);
  gen->strip[col] = square_is_empty ? ml : PLAYED_THROUGH_MARKER;
  woadwage_add_square_score(gen, col, ml, &main_word_score, &word_multiplier,
                            &cross_score);
  if (square_is_empty && gen->dir &&
      gen_cache_get_cross_set(gen, col) == TRIVIAL_CROSS_SET) {
    unique_play = true;
  }
  const int leftstrip = col;
  const bool no_letter_directly_left =
      col == 0 || gen_cache_is_empty(gen, col - 1);

  if (no_letter_directly_left &&
      play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play) &&
      woadwage_left_accepts(states)) {
    record_tile_placement_move(gen, leftstrip, rightstrip, main_word_score,
                               word_multiplier, cross_score);
  }

  if (col > 0 && col - 1 != gen->last_anchor_col) {
    WoadwageLeftStates continuing;
    woadwage_continue_left(gen->kwg, states, &continuing);
    if (continuing.count > 0) {
      woadwage_gen_left(gen, col - 1, &continuing, rightstrip, unique_play,
                        main_word_score, word_multiplier, cross_score);
    }
  }

  if (no_letter_directly_left && gen->current_anchor_col < BOARD_DIM - 1) {
    WoadwageRightStates right;
    woadwage_start_right(gen->kwg, states, &right);
    if (right.count > 0) {
      woadwage_gen_right(gen, gen->current_anchor_col + 1, &right, leftstrip,
                         unique_play, main_word_score, word_multiplier,
                         cross_score);
    }
  }
}

// With the letter ml on col, right of the anchor, and read into states:
// records the play if it ends here, then reads on to the right.
static void woadwage_go_on_right(MoveGen *gen, int col, MachineLetter ml,
                                 const WoadwageRightStates *states,
                                 int leftstrip, bool unique_play,
                                 Equity main_word_score, int word_multiplier,
                                 Equity cross_score) {
  if (gen->threshold_exceeded) {
    return;
  }
  const bool square_is_empty = gen_cache_is_empty(gen, col);
  gen->strip[col] = square_is_empty ? ml : PLAYED_THROUGH_MARKER;
  woadwage_add_square_score(gen, col, ml, &main_word_score, &word_multiplier,
                            &cross_score);
  if (square_is_empty && !unique_play && gen->dir &&
      gen_cache_get_cross_set(gen, col) == TRIVIAL_CROSS_SET) {
    unique_play = true;
  }
  const int rightstrip = col;
  const bool no_letter_directly_right =
      col == BOARD_DIM - 1 || gen_cache_is_empty(gen, col + 1);

  if (no_letter_directly_right &&
      play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play) &&
      woadwage_right_accepts(states)) {
    record_tile_placement_move(gen, leftstrip, rightstrip, main_word_score,
                               word_multiplier, cross_score);
  }

  if (col < BOARD_DIM - 1) {
    WoadwageRightStates continuing;
    woadwage_continue_right(gen->kwg, states, &continuing);
    if (continuing.count > 0) {
      woadwage_gen_right(gen, col + 1, &continuing, leftstrip, unique_play,
                         main_word_score, word_multiplier, cross_score);
    }
  }
}

void woadwage_gen_anchor(MoveGen *gen) {
  WoadwageLeftStates states;
  states.count = 1;
  states.states[0] = (WoadwageLeftState){
      .type = WOADWAGE_LEFT_ANCHOR_WORD,
      .node_index = kwg_get_root_node_index(gen->kwg),
      .accepts = false,
      .anchor_word_separator_node_index = 0,
      .anchor_word_accepts = false,
  };
  woadwage_gen_left(gen, gen->current_anchor_col, &states,
                    gen->current_anchor_col,
                    gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1, 0);
}
