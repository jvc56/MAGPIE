#include "move_gen.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/klv_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/anchor.h"
#include "../ent/bag.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/kwg_alpha.h"
#include "../ent/leave_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
#include "../compat/cpthread.h"
#include "../util/io_util.h"
#include "wmp_move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_LAST_ANCHOR_COL (BOARD_DIM)

// Cache move generators since destroying
// and recreating a movegen for
// every request to generate moves would
// be expensive. The infer and sim functions
// don't have this problem since they are
// only called once per command.
static MoveGen *cached_gens[MAX_THREADS];
static cpthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

MoveGen *generator_create(void) {
  MoveGen *generator = malloc_or_die(sizeof(MoveGen));
  generator->tiles_played = 0;
  generator->dir = BOARD_HORIZONTAL_DIRECTION;
  return generator;
}

void generator_destroy(MoveGen *gen) {
  if (!gen) {
    return;
  }
  free(gen);
}

MoveGen *get_movegen(int thread_index) {
  if (!cached_gens[thread_index]) {
    cached_gens[thread_index] = generator_create();
  }
  return cached_gens[thread_index];
}

// FIXME: is this necessary? the MoveGen struct may not have anymore
// dist-dependent heap alloc'd fields, so maybe we can now destroy only at
// program exit.
void gen_destroy_cache(void) {
  cpthread_mutex_lock(&cache_mutex);
  for (int i = 0; i < (MAX_THREADS); i++) {
    generator_destroy(cached_gens[i]);
    cached_gens[i] = NULL;
  }
  cpthread_mutex_unlock(&cache_mutex);
}

// Cache getter functions

static inline MachineLetter gen_cache_get_letter(const MoveGen *gen, int col) {
  return square_get_letter(&gen->row_cache[col]);
}

static inline bool gen_cache_get_is_anchor(const MoveGen *gen, int col) {
  return square_get_anchor(&gen->row_cache[col]);
}

static inline int gen_cache_is_empty(const MoveGen *gen, int col) {
  return gen_cache_get_letter(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static inline bool gen_cache_get_is_cross_word(const MoveGen *gen, int col) {
  return square_get_is_cross_word(&gen->row_cache[col]);
}

static inline BonusSquare gen_cache_get_bonus_square(const MoveGen *gen,
                                                     int col) {
  return square_get_bonus_square(&gen->row_cache[col]);
}

static inline uint64_t gen_cache_get_cross_set(const MoveGen *gen, int col) {
  return square_get_cross_set(&gen->row_cache[col]);
}

static inline Equity gen_cache_get_cross_score(const MoveGen *gen, int col) {
  return square_get_cross_score(&gen->row_cache[col]);
}

static inline uint64_t gen_cache_get_left_extension_set(const MoveGen *gen,
                                                        int col) {
  return square_get_left_extension_set(&gen->row_cache[col]);
}

static inline uint64_t gen_cache_get_right_extension_set(const MoveGen *gen,
                                                         int col) {
  return square_get_right_extension_set(&gen->row_cache[col]);
}

static inline Equity gen_get_static_equity(const MoveGen *gen,
                                           const Move *move) {
  return static_eval_get_move_equity_with_leave_value(
      &gen->ld, move, &gen->player_rack, &gen->opponent_rack,
      gen->opening_move_penalties, gen->board_number_of_tiles_played,
      gen->number_of_tiles_in_bag,
      leave_map_get_current_value(&gen->leave_map));
}

static inline const Move *gen_get_readonly_best_move(const MoveGen *gen) {
  return &gen->best_move_and_current_move[gen->best_move_index];
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

static inline void gen_update_cutoff_equity_or_score(MoveGen *gen) {
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_ALL_SMALL:
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
    return;
  case MOVE_RECORD_BEST:;
    const Move *move = gen_get_readonly_best_move(gen);
    gen->cutoff_equity_or_score = (gen->move_sort_type == MOVE_SORT_EQUITY)
                                      ? move_get_equity(move)
                                      : move_get_score(move);
  }
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
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:;
    Move *move = move_list_get_spare_move(gen->move_list);
    set_play_for_record(move, move_type, leftstrip, rightstrip, score,
                        start_row, start_col, tiles_played, dir, strip);
    const Equity move_equity_or_score =
        get_move_equity_for_sort_type(gen, move, score);
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
    move_set_equity(current_move,
                    get_move_equity_for_sort_type(gen, current_move, score));
    if (compare_moves(current_move, gen_get_readonly_best_move(gen), false)) {
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
  }
  if (need_to_update_best_move_equity_or_score) {
    gen_update_cutoff_equity_or_score(gen);
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
    return false;
    break;
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
    if (gen->best_move_equity_or_score == EQUITY_INITIAL_VALUE) {
      return false;
    }
    break;
  case MOVE_RECORD_BEST:
    break;
  }
  return highest_possible_value < gen_get_cutoff_equity_or_score(gen);
}

static inline void record_exchange(MoveGen *gen) {
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_ALL_SMALL:
    break;
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
  case MOVE_RECORD_BEST:
    if (gen->move_sort_type == MOVE_SORT_EQUITY) {
      const Equity leave_value = leave_map_get_current_value(&gen->leave_map);
      if (better_play_has_been_found(gen, leave_value)) {
        return;
      }
    }
    break;
  }

  int tiles_exchanged = 0;

  const uint16_t rack_dist_size = rack_get_dist_size(&gen->player_rack);

  for (uint16_t ml = 0; ml < rack_dist_size; ml++) {
    const uint16_t num_this = rack_get_letter(&gen->player_rack, ml);
    for (uint16_t i = 0; i < num_this; i++) {
      gen->exchange_strip[tiles_exchanged] = ml;
      tiles_exchanged++;
    }
  }

  update_best_move_or_insert_into_movelist(
      gen, 0, tiles_exchanged, GAME_EVENT_EXCHANGE, 0, 0, 0, tiles_exchanged,
      BOARD_HORIZONTAL_DIRECTION, gen->exchange_strip);
}

// Look up leave values for all subsets of the player's rack and if add_exchange
// is true, record exchange moves for them. KLV indices are retained to speed up
// lookup of leaves with common lexicographical "prefixes".
void generate_exchange_moves(MoveGen *gen, Rack *leave, uint32_t node_index,
                             uint32_t word_index, MachineLetter ml,
                             bool add_exchange) {
  const int ld_size = ld_get_size(&gen->ld);
  while (ml < ld_size && rack_get_letter(&gen->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int number_of_letters_on_rack =
        rack_get_total_letters(&gen->player_rack);
    if (number_of_letters_on_rack > 0) {
      Equity value = 0;
      if (word_index != KLV_UNFOUND_INDEX) {
        value = klv_get_indexed_leave_value(gen->klv, word_index - 1);
      }
      leave_map_set_current_value(&gen->leave_map, value);
      if (value > gen->best_leaves[rack_get_total_letters(leave)]) {
        gen->best_leaves[rack_get_total_letters(leave)] = value;
      }
      if (add_exchange) {
        record_exchange(gen);
      }
    }
  } else {
    generate_exchange_moves(gen, leave, node_index, word_index, ml + 1,
                            add_exchange);
    const uint16_t num_this = rack_get_letter(&gen->player_rack, ml);
    for (uint16_t i = 0; i < num_this; i++) {
      rack_add_letter(leave, ml);
      leave_map_take_letter_and_update_complement_index(&gen->leave_map,
                                                        &gen->player_rack, ml);
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(gen->klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index =
          follow_arc(gen->klv, node_index, word_index, &child_word_index);
      word_index = child_word_index;
      generate_exchange_moves(gen, leave, node_index, word_index, ml + 1,
                              add_exchange);
    }

    rack_take_letters(leave, ml, num_this);
    for (int i = 0; i < num_this; i++) {
      leave_map_add_letter_and_update_complement_index(&gen->leave_map,
                                                       &gen->player_rack, ml);
    }
  }
}

static inline void set_play_for_record_wmp(MoveGen *gen, Move *move,
                                           int start_col, int score) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  move_set_all_except_equity(
      move, gen->playthrough_marked, 0, wgen->word_length - 1, score,
      gen->current_row_index, start_col, gen->max_tiles_to_play, gen->dir,
      GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(gen->dir)) {
    move_set_row_start(move, start_col);
    move_set_col_start(move, gen->current_row_index);
  }
}

static inline Equity get_move_equity_for_sort_type_wmp(MoveGen *gen,
                                                       const Move *move,
                                                       Equity leave_value) {
  switch (gen->move_sort_type) {
  case MOVE_SORT_EQUITY:
    return static_eval_get_move_equity_with_leave_value(
        &gen->ld, move, &gen->leave, &gen->opponent_rack,
        gen->opening_move_penalties, gen->board_number_of_tiles_played,
        gen->number_of_tiles_in_bag, leave_value);
  case MOVE_SORT_SCORE:
    return move_get_score(move);
  default:
    // TODO(olaugh): make move_sort_type an enum and remove default case
    log_fatal("unhandled move sort type: %d", gen->move_sort_type);
  }
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#else
  return move_get_score(move);
#endif
}

static inline void
update_best_move_or_insert_into_movelist_wmp(MoveGen *gen, int start_col,
                                             int score, Equity leave_value) {
  bool need_to_update_best_move_equity_or_score = false;
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST: {
    Move *move = move_list_get_spare_move(gen->move_list);
    set_play_for_record_wmp(gen, move, start_col, score);
    const Equity move_equity_or_score =
        get_move_equity_for_sort_type_wmp(gen, move, leave_value);
    if (gen->move_record_type == MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST) {
      // This updates the cutoff move internally so no update will be pending
      // afterward.
      gen_insert_spare_move_within_x_equity_of_best(gen, move_equity_or_score);
    } else {
      move_list_insert_spare_move(gen->move_list, move_equity_or_score);
    }
    break;
  }
  case MOVE_RECORD_BEST: {
    Move *current_move = gen_get_current_move(gen);
    set_play_for_record_wmp(gen, current_move, start_col, score);
    move_set_equity(current_move, get_move_equity_for_sort_type_wmp(
                                      gen, current_move, leave_value));
    if (compare_moves(current_move, gen_get_readonly_best_move(gen), false)) {
      need_to_update_best_move_equity_or_score = true;
      gen_switch_best_move_and_current_move(gen);
    }
    break;
  }
  case MOVE_RECORD_ALL_SMALL:
    log_fatal("update_best_move_or_insert_into_movelist_wmp called with "
              "MOVE_RECORD_ALL_SMALL");
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
    __builtin_unreachable();
#else
    return;
#endif
  }
  if (need_to_update_best_move_equity_or_score) {
    gen_update_cutoff_equity_or_score(gen);
  }
}

void record_wmp_play(MoveGen *gen, int start_col, Equity leave_value) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const int bingo_bonus =
      gen->max_tiles_to_play == RACK_SIZE ? gen->bingo_bonus : 0;
  int played_score_total = 0;
  int playthrough_score_total = 0;
  // from already played letters hooked
  int hooked_cross_total = 0;
  // from newly played tiles counting both ways due to crossing
  int played_cross_total = 0;
  int word_multiplier = 1;
  for (int letter_idx = 0; letter_idx < wgen->word_length; letter_idx++) {
    const int board_col = letter_idx + start_col;
    MachineLetter ml = gen->playthrough_marked[letter_idx];
    if (ml != PLAYED_THROUGH_MARKER) {
      const int tile_score = get_is_blanked(ml) ? 0 : gen->tile_scores[ml];
      const BonusSquare bonus_square =
          gen_cache_get_bonus_square(gen, board_col);
      const int this_word_multiplier =
          bonus_square_get_word_multiplier(bonus_square);
      const int hooked_cross_score =
          gen_cache_get_cross_score(gen, board_col) * this_word_multiplier;
      hooked_cross_total += hooked_cross_score;
      const int is_cross_word =
          gen_cache_get_is_cross_word(gen, board_col) ? 1 : 0;
      const int letter_multiplier =
          bonus_square_get_letter_multiplier(bonus_square);
      played_cross_total +=
          is_cross_word * tile_score * this_word_multiplier * letter_multiplier;
      played_score_total += tile_score * letter_multiplier;
      word_multiplier *= this_word_multiplier;
    } else {
      ml = gen_cache_get_letter(gen, board_col);
      const int tile_score = get_is_blanked(ml) ? 0 : gen->tile_scores[ml];
      playthrough_score_total += tile_score;
    }
  }
  const int score =
      (played_score_total + playthrough_score_total) * word_multiplier +
      hooked_cross_total + played_cross_total + bingo_bonus;
  update_best_move_or_insert_into_movelist_wmp(gen, start_col, score,
                                               leave_value);
}

void get_blank_possibilities(const MoveGen *gen,
                             const BitRack *nonplaythrough_tiles,
                             int current_pos, bool *can_be_unblanked,
                             bool *can_be_blanked) {
  const MachineLetter target_letter = gen->playthrough_marked[current_pos];
  const int count_of_target_letter_in_subrack =
      bit_rack_get_letter(nonplaythrough_tiles, target_letter);

  int count_before_current_pos = 0;
  int count_at_or_after_current_pos = 0;
  for (int pos = 0; pos < gen->wmp_move_gen.word_length; pos++) {
    const MachineLetter playthrough_letter = gen->playthrough_marked[pos];
    if (playthrough_letter == PLAYED_THROUGH_MARKER ||
        get_is_blanked(playthrough_letter)) {
      // Previously placed board tiles and already-blanked letters do not
      // consume non-blank tiles from the subrack.
      continue;
    }
    if (playthrough_letter != target_letter) {
      continue;
    }
    if (pos < current_pos) {
      count_before_current_pos++;
    } else {
      count_at_or_after_current_pos++;
    }
  }

  const int total_nonblank_in_word =
      count_before_current_pos + count_at_or_after_current_pos;

  // We can place a non-blank tile here if we have one left on the subrack
  // after accounting for the ones required before this position.
  const int target_letter_remaining_on_rack =
      count_of_target_letter_in_subrack - count_before_current_pos;
  *can_be_unblanked = target_letter_remaining_on_rack > 0;

  // We can (and must) use a blank for this letter if the number of non-blank
  // tiles required by the word exceeds what is available on the subrack.
  *can_be_blanked = total_nonblank_in_word > count_of_target_letter_in_subrack;
}

void record_wmp_plays_for_word(MoveGen *gen, int subrack_idx, int start_col,
                               int blanks_so_far, int pos) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const BitRack *nonplaythrough_tiles =
      wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx);
  const int num_blanks =
      bit_rack_get_letter(nonplaythrough_tiles, BLANK_MACHINE_LETTER);
  if (num_blanks == blanks_so_far) {
    record_wmp_play(gen, start_col, wgen->leave_value);
    return;
  }
  assert(pos < wgen->word_length);
  const MachineLetter ml = gen->playthrough_marked[pos];
  if (ml == PLAYED_THROUGH_MARKER) {
    record_wmp_plays_for_word(gen, subrack_idx, start_col, blanks_so_far,
                              pos + 1);
    return;
  }
  bool can_be_unblanked;
  bool can_be_blanked;
  get_blank_possibilities(gen, nonplaythrough_tiles, pos, &can_be_unblanked,
                          &can_be_blanked);
  if (can_be_unblanked) {
    record_wmp_plays_for_word(gen, subrack_idx, start_col, blanks_so_far,
                              pos + 1);
  }
  if (can_be_blanked) {
    gen->playthrough_marked[pos] = get_blanked_machine_letter(ml);
    record_wmp_plays_for_word(gen, subrack_idx, start_col, blanks_so_far + 1,
                              pos + 1);
    gen->playthrough_marked[pos] = ml;
  }
}

bool wordmap_gen_check_playthrough_and_crosses(MoveGen *gen, int word_idx,
                                               int start_col) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const MachineLetter *word = wmp_move_gen_get_word(wgen, word_idx);
  for (int letter_idx = 0; letter_idx < wgen->word_length; letter_idx++) {
    const int board_col = start_col + letter_idx;
    assert(board_col < BOARD_DIM);
    assert(board_col >= 0);
    const MachineLetter word_letter = word[letter_idx];
    if (gen_cache_is_empty(gen, board_col)) {
      if (!board_is_letter_allowed_in_cross_set(
              gen_cache_get_cross_set(gen, board_col), word_letter)) {
        return false;
      }
      gen->playthrough_marked[letter_idx] = word_letter;
      continue;
    }
    const MachineLetter board_letter =
        get_unblanked_machine_letter(gen_cache_get_letter(gen, board_col));
    assert(board_letter != ALPHABET_EMPTY_SQUARE_MARKER);
    assert(!bonus_square_is_brick(gen_cache_get_bonus_square(gen, board_col)));
    if (board_letter != word_letter) {
      return false;
    }
    gen->playthrough_marked[letter_idx] = PLAYED_THROUGH_MARKER;
  }
  return true;
}

void wordmap_gen(MoveGen *gen, const Anchor *anchor) {
  assert(gen != NULL);
  assert(anchor != NULL);
  gen->max_tiles_to_play = anchor->tiles_to_play;
  assert(anchor->tiles_to_play <= rack_get_total_letters(&gen->player_rack));
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  wmp_move_gen_set_playthrough_bit_rack(wgen, anchor, gen->row_cache);
  wmp_move_gen_playthrough_subracks_init(wgen, anchor);

  assert(anchor->leftmost_start_col <= anchor->rightmost_start_col);
  assert(anchor->leftmost_start_col <= anchor->col);
  assert(anchor->rightmost_start_col <= anchor->col);
  const int num_subrack_combinations =
      wmp_move_gen_get_num_subrack_combinations(wgen);
  for (int subrack_idx = 0; subrack_idx < num_subrack_combinations;
       subrack_idx++) {
    if (gen->number_of_tiles_in_bag > 0) {
      const Equity leave_value =
          wmp_move_gen_get_leave_value(wgen, subrack_idx);
      if (better_play_has_been_found(gen, leave_value +
                                              anchor->highest_possible_score)) {
        continue;
      }
    }
    if (!wmp_move_gen_get_subrack_words(wgen, subrack_idx)) {
      continue;
    }
    if (gen->number_of_tiles_in_bag == 0) {
      wgen->leave_value = 0;
      for (int ml = 0; ml < ld_get_size(&gen->ld); ml++) {
        const int leave_num_ml =
            rack_get_letter(&gen->player_rack, ml) -
            bit_rack_get_letter(
                wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx), ml);
        rack_set_letter(&gen->leave, ml, leave_num_ml);
      }
      rack_set_total_letters(&gen->leave,
                             rack_get_total_letters(&gen->player_rack) -
                                 anchor->tiles_to_play);
    }
    for (int word_idx = 0; word_idx < wgen->num_words; word_idx++) {
      for (int start_col = anchor->leftmost_start_col;
           start_col <= anchor->rightmost_start_col; start_col++) {
        if (wordmap_gen_check_playthrough_and_crosses(gen, word_idx,
                                                      start_col)) {
          record_wmp_plays_for_word(gen, subrack_idx, start_col, 0, 0);
        }
      }
    }
  }
}

void go_on(MoveGen *gen, int current_col, MachineLetter L,
           uint32_t new_node_index, bool accepts, int leftstrip, int rightstrip,
           bool unique_play, int main_word_score, int word_multiplier,
           int cross_score);

void recursive_gen(MoveGen *gen, int col, uint32_t node_index, int leftstrip,
                   int rightstrip, bool unique_play, int main_word_score,
                   int word_multiplier, int cross_score) {
  const MachineLetter current_letter = gen_cache_get_letter(gen, col);
  // If current_letter is nonempty, leftx is the set of letters that could go
  // immediately left of the current block of played tiles.
  //
  // If current_letter is empty, leftx is the set of letters that could
  // go immediately left of the block of tiles to the right of this square.
  //
  // In either case, it is a mask applied to the tile that could be placed by
  // this function.
  uint64_t possible_letters_here = gen_cache_get_cross_set(gen, col) &
                                   gen_cache_get_left_extension_set(gen, col);
  if ((gen->tiles_played == 0) && (col == gen->current_anchor_col + 1)) {
    possible_letters_here &= gen->anchor_right_extension_set;
  }
  if (possible_letters_here == 1) {
    possible_letters_here = 0;
  }
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const MachineLetter raw = get_unblanked_machine_letter(current_letter);
    uint32_t next_node_index = 0;
    bool accepts = false;
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      if (kwg_node_tile(node) == raw) {
        next_node_index = kwg_node_arc_index_prefetch(node, gen->kwg);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    go_on(gen, col, current_letter, next_node_index, accepts, leftstrip,
          rightstrip, unique_play, main_word_score, word_multiplier,
          cross_score);
  } else if (!rack_is_empty(&gen->player_rack) &&
             ((possible_letters_here & gen->rack_cross_set) != 0)) {
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      const MachineLetter ml = kwg_node_tile(node);
      const uint16_t number_of_ml = rack_get_letter(&gen->player_rack, ml);
      if (ml != 0 &&
          (number_of_ml != 0 ||
           rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) != 0) &&
          board_is_letter_allowed_in_cross_set(possible_letters_here, ml)) {
        const uint32_t next_node_index =
            kwg_node_arc_index_prefetch(node, gen->kwg);
        bool accepts = kwg_node_accepts(node);
        if (number_of_ml > 0) {
          leave_map_take_letter_and_update_current_index(&gen->leave_map,
                                                         &gen->player_rack, ml);
          gen->tiles_played++;
          go_on(gen, col, ml, next_node_index, accepts, leftstrip, rightstrip,
                unique_play, main_word_score, word_multiplier, cross_score);
          gen->tiles_played--;
          leave_map_add_letter_and_update_current_index(&gen->leave_map,
                                                        &gen->player_rack, ml);
        }
        // check blank
        if (rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) > 0) {
          leave_map_take_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, BLANK_MACHINE_LETTER);
          gen->tiles_played++;
          go_on(gen, col, get_blanked_machine_letter(ml), next_node_index,
                accepts, leftstrip, rightstrip, unique_play, main_word_score,
                word_multiplier, cross_score);
          gen->tiles_played--;
          leave_map_add_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
}

static inline bool play_is_nonempty_and_nonduplicate(int tiles_played,
                                                     bool is_unique) {
  return (tiles_played > 1) || ((tiles_played == 1) && is_unique);
}

void go_on(MoveGen *gen, int current_col, MachineLetter L,
           uint32_t new_node_index, bool accepts, int leftstrip, int rightstrip,
           bool unique_play, int main_word_score, int word_multiplier,
           int cross_score) {
  // Handle incremental scoring
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, current_col);
  uint8_t letter_multiplier = 1;
  uint8_t this_word_multiplier = 1;
  bool fresh_tile = false;

  const bool square_is_empty = gen_cache_is_empty(gen, current_col);
  MachineLetter ml;
  if (!square_is_empty) {
    gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    ml = gen_cache_get_letter(gen, current_col);
  } else {
    gen->strip[current_col] = L;
    ml = L;
    fresh_tile = true;
    this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
  }

  int inc_word_multiplier = this_word_multiplier * word_multiplier;

  const int lsm = gen->tile_scores[ml] * letter_multiplier;

  int inc_main_word_score = lsm + main_word_score;

  int inc_cross_scores = cross_score;

  if (fresh_tile && gen_cache_get_is_cross_word(gen, current_col)) {
    inc_cross_scores += (lsm + gen_cache_get_cross_score(gen, current_col)) *
                        this_word_multiplier;
  }

  if (current_col <= gen->current_anchor_col) {
    if (square_is_empty && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || gen_cache_is_empty(gen, current_col - 1);

    if (accepts && no_letter_directly_left &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (new_node_index == 0) {
      return;
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen(gen, current_col - 1, new_node_index, leftstrip, rightstrip,
                    unique_play, inc_main_word_score, inc_word_multiplier,
                    inc_cross_scores);
    }

    // rightx only tells you which tiles can go to the right of a string which
    // begins a word. So it can only filter here if no tiles have been played
    // so far. If gen->tiles_played is 0, this recursive_gen call would be
    // placing the first tile of a play and continuing to the right.
    if ((gen->tiles_played != 0) ||
        (gen->anchor_right_extension_set & gen->rack_cross_set) != 0) {
      uint32_t separation_node_index = kwg_get_next_node_index(
          gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
      if (separation_node_index != 0 && no_letter_directly_left &&
          gen->current_anchor_col < BOARD_DIM - 1) {
        recursive_gen(gen, gen->current_anchor_col + 1, separation_node_index,
                      leftstrip, rightstrip, unique_play, inc_main_word_score,
                      inc_word_multiplier, inc_cross_scores);
      }
    }
  } else {
    if (square_is_empty && !unique_play && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    rightstrip = current_col;
    bool no_letter_directly_right = (current_col == BOARD_DIM - 1) ||
                                    gen_cache_is_empty(gen, current_col + 1);

    if (accepts && no_letter_directly_right &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen(gen, current_col + 1, new_node_index, leftstrip, rightstrip,
                    unique_play, inc_main_word_score, inc_word_multiplier,
                    inc_cross_scores);
    }
  }
}

void go_on_alpha(MoveGen *gen, int current_col, MachineLetter L, int leftstrip,
                 int rightstrip, bool unique_play, int main_word_score,
                 int word_multiplier, int cross_score);

void recursive_gen_alpha(MoveGen *gen, int col, int leftstrip, int rightstrip,
                         bool unique_play, int main_word_score,
                         int word_multiplier, int cross_score) {
  const MachineLetter current_letter = gen_cache_get_letter(gen, col);
  uint64_t possible_letters_here = gen_cache_get_cross_set(gen, col);
  if (possible_letters_here == 1) {
    possible_letters_here = 0;
  }
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const MachineLetter raw = get_unblanked_machine_letter(current_letter);
    rack_add_letter(&gen->full_player_rack, raw);
    go_on_alpha(gen, col, current_letter, leftstrip, rightstrip, unique_play,
                main_word_score, word_multiplier, cross_score);
    rack_take_letter(&gen->full_player_rack, raw);
  } else if (!rack_is_empty(&gen->player_rack) &&
             ((possible_letters_here & gen->rack_cross_set) != 0)) {
    const MachineLetter ld_size = ld_get_size(&gen->ld);
    for (MachineLetter ml = 1; ml < ld_size; ml++) {
      const uint16_t number_of_ml = rack_get_letter(&gen->player_rack, ml);
      if ((number_of_ml != 0 ||
           rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) != 0) &&
          board_is_letter_allowed_in_cross_set(possible_letters_here, ml)) {
        if (number_of_ml > 0) {
          alpha_leave_map_take_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack, ml,
              ml);
          gen->tiles_played++;
          go_on_alpha(gen, col, ml, leftstrip, rightstrip, unique_play,
                      main_word_score, word_multiplier, cross_score);
          gen->tiles_played--;
          alpha_leave_map_add_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack, ml,
              ml);
        }
        // check blank
        if (rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) > 0) {
          alpha_leave_map_take_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack,
              BLANK_MACHINE_LETTER, ml);
          gen->tiles_played++;
          go_on_alpha(gen, col, get_blanked_machine_letter(ml), leftstrip,
                      rightstrip, unique_play, main_word_score, word_multiplier,
                      cross_score);
          gen->tiles_played--;
          alpha_leave_map_add_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack,
              BLANK_MACHINE_LETTER, ml);
        }
      }
    }
  }
}

void go_on_alpha(MoveGen *gen, int current_col, MachineLetter L, int leftstrip,
                 int rightstrip, bool unique_play, int main_word_score,
                 int word_multiplier, int cross_score) {
  // Handle incremental scoring
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, current_col);
  int letter_multiplier = 1;
  int this_word_multiplier = 1;
  bool fresh_tile = false;

  const bool square_is_empty = gen_cache_is_empty(gen, current_col);
  MachineLetter ml;
  if (!square_is_empty) {
    gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    ml = gen_cache_get_letter(gen, current_col);
  } else {
    gen->strip[current_col] = L;
    ml = L;
    fresh_tile = true;
    this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
  }

  int inc_word_multiplier = this_word_multiplier * word_multiplier;

  const int lsm = gen->tile_scores[ml] * letter_multiplier;

  int inc_main_word_score = lsm + main_word_score;

  int inc_cross_scores = cross_score;

  if (fresh_tile && gen_cache_get_is_cross_word(gen, current_col)) {
    inc_cross_scores += (lsm + gen_cache_get_cross_score(gen, current_col)) *
                        this_word_multiplier;
  }

  const bool accepts = kwg_accepts_alpha(gen->kwg, &gen->full_player_rack);
  if (current_col <= gen->current_anchor_col) {
    if (square_is_empty && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || gen_cache_is_empty(gen, current_col - 1);

    if (accepts && no_letter_directly_left &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen_alpha(gen, current_col - 1, leftstrip, rightstrip,
                          unique_play, inc_main_word_score, inc_word_multiplier,
                          inc_cross_scores);
    }

    // rightx only tells you which tiles can go to the right of a string which
    // begins a word. So it can only filter here if no tiles have been played
    // so far. If gen->tiles_played is 0, this recursive_gen call would be
    // placing the first tile of a play and continuing to the right.
    if ((gen->tiles_played != 0) ||
        (gen->anchor_right_extension_set & gen->rack_cross_set) != 0) {
      if (no_letter_directly_left && gen->current_anchor_col < BOARD_DIM - 1) {
        recursive_gen_alpha(gen, gen->current_anchor_col + 1, leftstrip,
                            rightstrip, unique_play, inc_main_word_score,
                            inc_word_multiplier, inc_cross_scores);
      }
    }
  } else {
    if (square_is_empty && !unique_play && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    rightstrip = current_col;
    bool no_letter_directly_right = (current_col == BOARD_DIM - 1) ||
                                    gen_cache_is_empty(gen, current_col + 1);

    if (accepts && no_letter_directly_right &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (current_col < BOARD_DIM - 1) {
      recursive_gen_alpha(gen, current_col + 1, leftstrip, rightstrip,
                          unique_play, inc_main_word_score, inc_word_multiplier,
                          inc_cross_scores);
    }
  }
}

static inline void shadow_record(MoveGen *gen) {
  const Equity *best_leaves = gen->best_leaves;
  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    if (wmp_move_gen_has_playthrough(&gen->wmp_move_gen) &&
        (gen->tiles_played == gen->number_of_letters_on_rack)) {
      if (!wmp_move_gen_check_playthrough_full_rack_existence(
              &gen->wmp_move_gen)) {
        return;
      }
    }
    if (!wmp_move_gen_has_playthrough(&gen->wmp_move_gen) &&
        (gen->tiles_played >= MINIMUM_WORD_LENGTH)) {
      if (!wmp_move_gen_nonplaythrough_word_of_length_exists(
              &gen->wmp_move_gen, gen->tiles_played)) {
        return;
      }
      if (gen->number_of_tiles_in_bag > 0) {
        best_leaves = wmp_move_gen_get_nonplaythrough_best_leave_values(
            &gen->wmp_move_gen);
        for (int i = 0; i < RACK_SIZE - gen->tiles_played; i++) {
          assert(best_leaves[i] <= gen->best_leaves[i]);
        }
      }
    }
  }

  if (gen->is_wordsmog && (gen->tiles_played == RACK_SIZE) &&
      !kwg_accepts_alpha_with_blanks(gen->kwg, &gen->bingo_alpha_rack)) {
    return;
  }
  Equity tiles_played_score = 0;
  for (int i = 0; i < RACK_SIZE; i++) {
    tiles_played_score += gen->descending_tile_scores[i] *
                          gen->descending_effective_letter_multipliers[i];
  }

  Equity bingo_bonus = 0;
  if (gen->tiles_played == RACK_SIZE) {
    bingo_bonus = gen->bingo_bonus;
  }

  const Equity score =
      tiles_played_score +
      (gen->shadow_mainword_restricted_score * gen->shadow_word_multiplier) +
      gen->shadow_perpendicular_additional_score + bingo_bonus;
  Equity equity = score;
  if (gen->move_sort_type == MOVE_SORT_EQUITY) {
    equity += static_eval_get_shadow_equity(
        &gen->ld, &gen->opponent_rack, best_leaves,
        gen->full_rack_descending_tile_scores, gen->number_of_tiles_in_bag,
        gen->number_of_letters_on_rack, gen->tiles_played);
  }
  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    const int word_length =
        gen->wmp_move_gen.num_tiles_played_through + gen->tiles_played;
    if (word_length >= MINIMUM_WORD_LENGTH) {
      wmp_move_gen_maybe_update_anchor(&gen->wmp_move_gen, gen->tiles_played,
                                       word_length, gen->current_left_col,
                                       score, equity);
    }
  }
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
  if (score > gen->highest_shadow_score) {
    gen->highest_shadow_score = score;
  }
  if (gen->tiles_played > gen->max_tiles_to_play) {
    gen->max_tiles_to_play = gen->tiles_played;
  }
}

static inline void insert_unrestricted_cross_word_multiplier(MoveGen *gen,
                                                             uint8_t multiplier,
                                                             int col) {
  int insert_index = gen->num_unrestricted_multipliers;
  for (; insert_index > 0 &&
         gen->descending_cross_word_multipliers[insert_index - 1].multiplier <
             multiplier;
       insert_index--) {
    gen->descending_cross_word_multipliers[insert_index] =
        gen->descending_cross_word_multipliers[insert_index - 1];
  }
  gen->descending_cross_word_multipliers[insert_index].multiplier = multiplier;
  gen->descending_cross_word_multipliers[insert_index].column = col;
}

static inline void
insert_unrestricted_effective_letter_multiplier(MoveGen *gen,
                                                uint8_t multiplier) {
  int insert_index = gen->num_unrestricted_multipliers;
  for (; insert_index > 0 &&
         gen->descending_effective_letter_multipliers[insert_index - 1] <
             multiplier;
       insert_index--) {
    gen->descending_effective_letter_multipliers[insert_index] =
        gen->descending_effective_letter_multipliers[insert_index - 1];
  }
  gen->descending_effective_letter_multipliers[insert_index] = multiplier;
}

// Recalculate and reinsert all unrestricted effective letter multipliers,
// triggered by a change in the word multiplier.
static inline void maybe_recalculate_effective_multipliers(MoveGen *gen) {
  if (gen->last_word_multiplier == gen->shadow_word_multiplier) {
    return;
  }
  gen->last_word_multiplier = gen->shadow_word_multiplier;

  const int original_num_unrestricted_multipliers =
      gen->num_unrestricted_multipliers;
  gen->num_unrestricted_multipliers = 0;
  // We insert the columns with highest cross wordletter multipliers first
  // so the list will mostly sort in the the order it is traversed,
  // minimizing the number of swaps.
  for (int i = 0; i < original_num_unrestricted_multipliers; i++) {
    const uint8_t xw_multiplier =
        gen->descending_cross_word_multipliers[i].multiplier;
    const uint8_t col = gen->descending_cross_word_multipliers[i].column;
    const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, col);
    const uint8_t letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const uint8_t effective_letter_multiplier =
        gen->shadow_word_multiplier * letter_multiplier + xw_multiplier;
    insert_unrestricted_effective_letter_multiplier(
        gen, effective_letter_multiplier);
    gen->num_unrestricted_multipliers++;
  }
}

static inline void insert_unrestricted_multipliers(MoveGen *gen, int col) {
  maybe_recalculate_effective_multipliers(gen);

  const bool is_cross_word = gen_cache_get_is_cross_word(gen, col);
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, col);
  const uint8_t this_word_multiplier =
      bonus_square_get_word_multiplier(bonus_square);
  const uint8_t letter_multiplier =
      bonus_square_get_letter_multiplier(bonus_square);
  const uint8_t effective_cross_word_multiplier =
      letter_multiplier * this_word_multiplier * is_cross_word;
  insert_unrestricted_cross_word_multiplier(
      gen, effective_cross_word_multiplier, col);
  const uint8_t main_word_multiplier =
      gen->shadow_word_multiplier * letter_multiplier;
  insert_unrestricted_effective_letter_multiplier(
      gen, main_word_multiplier + effective_cross_word_multiplier);
  gen->num_unrestricted_multipliers++;
}

static inline bool is_single_bit_set(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcountll)
  return __builtin_popcountll(bitset) == 1;
#else
  return bitset && !(bitset & (bitset - 1));
#endif
}

static inline int get_single_bit_index(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bitset);
#else
  // Probably not the fastest fallback but it does well because it often
  // finds the blank's bit the first time through the loop.
  // Beware this infinite loops if bitset is zero.
  int index = 0;
  while ((bitset & 1) == 0) {
    bitset >>= 1;
    index++;
  }
  return index;
#endif
}

// descending_tile_scores is a list of tile scores in descending order.
// find and remove this score, shift the rest of the list to the left.
// fill the last element with 0.
static inline void remove_score_from_descending_tile_scores(MoveGen *gen,
                                                            Equity score) {
  // The tile has already been removed from the rack, so num_available_tiles
  // is actually the desired new size we want to be in sync with.
  const int num_available_tiles = rack_get_total_letters(&gen->player_rack);
  for (int i = num_available_tiles; i-- > 0;) {
    if (gen->descending_tile_scores[i] == score) {
      for (int j = i; j < num_available_tiles; j++) {
        gen->descending_tile_scores[j] = gen->descending_tile_scores[j + 1];
      }
      gen->descending_tile_scores[num_available_tiles] = 0;
      break;
    }
  }
}

// Returns false if the tile can not be restricted.
// Otherwise it removes the tile from the rack and adds the score.
static inline bool try_restrict_tile_and_accumulate_score(
    MoveGen *gen, uint64_t possible_letters_here, int letter_multiplier,
    int this_word_multiplier, int col) {
  const bool restricted = is_single_bit_set(possible_letters_here);
  if (!restricted) {
    return false;
  }
  const MachineLetter ml = get_single_bit_index(possible_letters_here);
  rack_take_letter(&gen->player_rack, ml);
  if (rack_get_letter(&gen->player_rack, ml) == 0) {
    gen->rack_cross_set &= ~possible_letters_here;
  }
  const bool is_cross_word = gen_cache_get_is_cross_word(gen, col);
  const Equity tile_score = gen->tile_scores[ml];
  remove_score_from_descending_tile_scores(gen, tile_score);
  const Equity lsm = tile_score * letter_multiplier;
  gen->shadow_mainword_restricted_score += lsm;

  // Equivalent to
  //
  //   gen->shadow_perpendicular_additional_score +=
  //     is_cross_word ? (lsm * this_word_multiplier) : 0;
  //
  // But verified with godbolt that this compiles to faster code.
  gen->shadow_perpendicular_additional_score +=
      (lsm * this_word_multiplier) & (-(int)is_cross_word);

  return true;
}

static inline void shadow_play_right(MoveGen *gen, bool is_unique) {
  // Save the score totals to be reset after shadowing right.
  const Equity orig_main_restricted_score =
      gen->shadow_mainword_restricted_score;
  const Equity orig_perp_score = gen->shadow_perpendicular_additional_score;
  const int orig_wordmul = gen->shadow_word_multiplier;

  // Save the rack with the tiles available before beginning shadow right. Any
  // tiles restricted by unique hooks will be returned to the rack after
  // exhausting rightward shadow.
  rack_copy(&gen->player_rack_shadow_right_copy, &gen->player_rack);
  rack_copy(&gen->bingo_alpha_rack_shadow_right_copy, &gen->bingo_alpha_rack);
  const uint64_t orig_rack_cross_set = gen->rack_cross_set;
  memcpy(gen->descending_tile_scores_copy, gen->descending_tile_scores,
         sizeof(gen->descending_tile_scores));
  // Only recopy the originals if a restriction modified the arrays above.
  bool restricted_any_tiles = false;

  // Save the state of the unrestricted multiplier arrays so they can be
  // restored after exhausting the rightward shadow plays. Shadowing right
  // changes the values of multipliers found while shadowing left. We need to
  // restore them to how they were before looking further left.
  const int orig_num_unrestricted_multipliers =
      gen->num_unrestricted_multipliers;
  memcpy(gen->desc_xw_muls_copy, gen->descending_cross_word_multipliers,
         sizeof(gen->descending_cross_word_multipliers));
  memcpy(gen->desc_eff_letter_muls_copy,
         gen->descending_effective_letter_multipliers,
         sizeof(gen->descending_effective_letter_multipliers));
  bool changed_any_restricted_multipliers = false;

  const int original_current_right_col = gen->current_right_col;
  const int original_tiles_played = gen->tiles_played;
  wmp_move_gen_save_playthrough_state(&gen->wmp_move_gen);

  while (gen->current_right_col < (BOARD_DIM - 1) &&
         gen->tiles_played < gen->number_of_letters_on_rack) {
    gen->current_right_col++;
    gen->tiles_played++;

    const uint64_t cross_set =
        gen_cache_get_cross_set(gen, gen->current_right_col);
    // If there are tiles to the right of this empty square, this holds a bitset
    // of the tiles that could possibly extend that sequence one square to the
    // left. Otherwise current_leftx will just be TRIVIAL_CROSS_SET.
    const uint64_t current_leftx =
        gen_cache_get_left_extension_set(gen, gen->current_right_col);
    const uint64_t nonblank_leftx =
        current_leftx & ~(1ULL << BLANK_MACHINE_LETTER);
    const uint64_t nonblank_cross_set =
        cross_set & ~(1ULL << BLANK_MACHINE_LETTER);
    // Which *letters* can be played here? Here we do not consider the blank to
    // be a letter. The letter it is designated must be compatible with both the
    // cross set and the left extension set.
    if ((nonblank_cross_set & nonblank_leftx) == 0) {
      break;
    }
    const uint64_t possible_letters_here = cross_set & gen->rack_cross_set &
                                           gen->anchor_right_extension_set &
                                           current_leftx;
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;
    if (possible_letters_here == 0) {
      break;
    }
    if (gen->tiles_played == 1) {
      gen->first_played_tile_col = gen->current_right_col;
    }

    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_right_col);
    const Equity cross_score =
        gen_cache_get_cross_score(gen, gen->current_right_col);
    const int letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const int this_word_multiplier =
        bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_perpendicular_additional_score +=
        cross_score * this_word_multiplier;
    gen->shadow_word_multiplier *= this_word_multiplier;

    if (try_restrict_tile_and_accumulate_score(
            gen, possible_letters_here, letter_multiplier, this_word_multiplier,
            gen->current_right_col)) {
      restricted_any_tiles = true;
    } else {
      insert_unrestricted_multipliers(gen, gen->current_right_col);
      changed_any_restricted_multipliers = true;
    }
    if (cross_set == TRIVIAL_CROSS_SET) {
      is_unique = true;
    }
    bool found_playthrough_tile = false;
    while (gen->current_right_col + 1 < BOARD_DIM) {
      const MachineLetter next_letter =
          gen_cache_get_letter(gen, gen->current_right_col + 1);
      if (next_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        break;
      }
      found_playthrough_tile = true;
      const MachineLetter unblanked_playthrough_ml =
          get_unblanked_machine_letter(next_letter);
      rack_add_letter(&gen->bingo_alpha_rack, unblanked_playthrough_ml);
      // Adding a letter here would be unsafe if the LetterDistribution's
      // alphabet size exceeded BIT_RACK_MAX_ALPHABET_SIZE.
      if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
        wmp_move_gen_add_playthrough_letter(&gen->wmp_move_gen,
                                            unblanked_playthrough_ml);
      }
      gen->shadow_mainword_restricted_score += gen->tile_scores[next_letter];
      gen->current_right_col++;
    }

    if (wmp_move_gen_is_active(&gen->wmp_move_gen) && found_playthrough_tile) {
      wmp_move_gen_increment_playthrough_blocks(&gen->wmp_move_gen);
    }

    if (play_is_nonempty_and_nonduplicate(gen->tiles_played, is_unique)) {
      // word_multiplier may have changed while playing through restricted
      // squares, in which case the restricted multiplier squares would
      // be invalidated.
      maybe_recalculate_effective_multipliers(gen);
      shadow_record(gen);
    }
  }

  // Restore state for score totals
  gen->shadow_mainword_restricted_score = orig_main_restricted_score;
  gen->shadow_perpendicular_additional_score = orig_perp_score;
  gen->shadow_word_multiplier = orig_wordmul;

  // Restore state for restricted squares
  if (restricted_any_tiles) {
    rack_copy(&gen->player_rack, &gen->player_rack_shadow_right_copy);
    gen->rack_cross_set = orig_rack_cross_set;
    memcpy(gen->descending_tile_scores, gen->descending_tile_scores_copy,
           sizeof(gen->descending_tile_scores));
  }

  // Restore state for unrestricted squares
  if (changed_any_restricted_multipliers) {
    gen->num_unrestricted_multipliers = orig_num_unrestricted_multipliers;
    memcpy(gen->descending_cross_word_multipliers, gen->desc_xw_muls_copy,
           sizeof(gen->descending_cross_word_multipliers));
    memcpy(gen->descending_effective_letter_multipliers,
           gen->desc_eff_letter_muls_copy,
           sizeof(gen->descending_effective_letter_multipliers));
  }

  // Restore state to undo other shadow progress
  gen->current_right_col = original_current_right_col;
  gen->tiles_played = original_tiles_played;
  rack_copy(&gen->bingo_alpha_rack, &gen->bingo_alpha_rack_shadow_right_copy);
  wmp_move_gen_restore_playthrough_state(&gen->wmp_move_gen);

  // The change of shadow_word_multiplier necessitates recalculating effective
  // multipliers.
  maybe_recalculate_effective_multipliers(gen);
}

static inline void nonplaythrough_shadow_play_left(MoveGen *gen,
                                                   bool is_unique) {
  for (;;) {
    const uint64_t possible_tiles_for_shadow_right =
        gen->anchor_right_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_right != 0) {
      shadow_play_right(gen, is_unique);
    }
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;
    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1 ||
        gen->tiles_played >= gen->number_of_letters_on_rack) {
      return;
    }
    const uint64_t possible_tiles_for_shadow_left =
        gen->anchor_left_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_left == 0) {
      return;
    }
    gen->anchor_left_extension_set = TRIVIAL_CROSS_SET;

    gen->current_left_col--;
    gen->tiles_played++;
    assert(gen->tiles_played > 1);
    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_left_col);
    int letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
    int this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_word_multiplier *= this_word_multiplier;
    if (!try_restrict_tile_and_accumulate_score(
            gen, possible_tiles_for_shadow_left, letter_multiplier,
            this_word_multiplier, gen->current_left_col)) {
      insert_unrestricted_multipliers(gen, gen->current_left_col);
    }
    shadow_record(gen);
  }
}

static inline void playthrough_shadow_play_left(MoveGen *gen, bool is_unique) {
  for (;;) {
    const uint64_t possible_tiles_for_shadow_right =
        gen->anchor_right_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_right != 0) {
      shadow_play_right(gen, is_unique);
    }
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;

    uint64_t possible_tiles_for_shadow_left =
        gen->anchor_left_extension_set & gen->rack_cross_set;
    const uint64_t nonblank_leftx =
        gen->anchor_left_extension_set & ~(1ULL << BLANK_MACHINE_LETTER);
    gen->anchor_left_extension_set = TRIVIAL_CROSS_SET;

    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1 ||
        gen->tiles_played >= gen->number_of_letters_on_rack) {
      break;
    }
    if (possible_tiles_for_shadow_left == 0) {
      break;
    }
    gen->current_left_col--;
    gen->tiles_played++;
    const uint64_t cross_set =
        gen_cache_get_cross_set(gen, gen->current_left_col);
    const uint64_t nonblank_cross_set =
        cross_set & ~(1ULL << BLANK_MACHINE_LETTER);
    // Which *letters* can be played here? Here we do not consider the blank to
    // be a letter. The letter it is designated must be compatible with both the
    // cross set and the left extension set.
    if ((nonblank_cross_set & nonblank_leftx) == 0) {
      break;
    }
    possible_tiles_for_shadow_left &= cross_set;
    if (possible_tiles_for_shadow_left == 0) {
      break;
    }
    if (gen->tiles_played == 1) {
      gen->first_played_tile_col = gen->current_left_col;
    }

    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_left_col);
    const Equity cross_score =
        gen_cache_get_cross_score(gen, gen->current_left_col);
    const int letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const int this_word_multiplier =
        bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_perpendicular_additional_score +=
        cross_score * this_word_multiplier;

    gen->shadow_word_multiplier *= this_word_multiplier;
    if (!try_restrict_tile_and_accumulate_score(
            gen, possible_tiles_for_shadow_left, letter_multiplier,
            this_word_multiplier, gen->current_left_col)) {
      insert_unrestricted_multipliers(gen, gen->current_left_col);
    }

    if (cross_set == TRIVIAL_CROSS_SET) {
      // See equivalent in shadow_play_right for the reasoning here.
      is_unique = true;
    }

    if (play_is_nonempty_and_nonduplicate(gen->tiles_played, is_unique)) {
      shadow_record(gen);
    }
  }
}

static inline void shadow_start_nonplaythrough(MoveGen *gen) {
  const uint64_t cross_set =
      gen_cache_get_cross_set(gen, gen->current_left_col);
  gen->first_played_tile_col = gen->current_left_col;
  const uint64_t possible_letters_here = cross_set & gen->rack_cross_set;
  // Only play a letter if a letter from the rack fits in the cross set
  if (possible_letters_here == 0) {
    return;
  }

  // Play tile on empty anchor square and set scoring parameters
  const BonusSquare bonus_square =
      gen_cache_get_bonus_square(gen, gen->current_left_col);
  const Equity cross_score =
      gen_cache_get_cross_score(gen, gen->current_left_col);
  const int letter_multiplier =
      bonus_square_get_letter_multiplier(bonus_square);
  const int this_word_multiplier =
      bonus_square_get_word_multiplier(bonus_square);
  gen->shadow_perpendicular_additional_score =
      cross_score * this_word_multiplier;

  // Temporarily set to zero, to not score in the other direction.
  gen->shadow_word_multiplier = 0;
  if (!try_restrict_tile_and_accumulate_score(
          gen, possible_letters_here, letter_multiplier, this_word_multiplier,
          gen->current_left_col)) {
    insert_unrestricted_multipliers(gen, gen->current_left_col);
  }
  gen->tiles_played++;
  if (!board_is_dir_vertical(gen->dir)) {
    // word_multiplier is always hard-coded as 0 since we are recording a
    // single tile
    shadow_record(gen);
  }
  gen->shadow_word_multiplier = this_word_multiplier;
  maybe_recalculate_effective_multipliers(gen);

  nonplaythrough_shadow_play_left(gen, !board_is_dir_vertical(gen->dir));
}

static inline void shadow_start_playthrough(MoveGen *gen,
                                            MachineLetter current_letter) {
  // Traverse the full length of the tiles on the board until hitting an
  // empty square
  for (;;) {
    const MachineLetter unblanked_playthrough_ml =
        get_unblanked_machine_letter(current_letter);
    rack_add_letter(&gen->bingo_alpha_rack, unblanked_playthrough_ml);
    if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
      wmp_move_gen_add_playthrough_letter(&gen->wmp_move_gen,
                                          unblanked_playthrough_ml);
    }
    gen->shadow_mainword_restricted_score += gen->tile_scores[current_letter];
    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1) {
      break;
    }
    gen->current_left_col--;
    current_letter = gen_cache_get_letter(gen, gen->current_left_col);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      gen->current_left_col++;
      break;
    }
  }
  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    wmp_move_gen_increment_playthrough_blocks(&gen->wmp_move_gen);
  }
  playthrough_shadow_play_left(gen, !board_is_dir_vertical(gen->dir));
}

static inline void shadow_start(MoveGen *gen) {
  const uint64_t any_extension_set =
      gen->anchor_left_extension_set | gen->anchor_right_extension_set;
  if (any_extension_set == 0) {
    return;
  }

  const uint64_t original_rack_cross_set = gen->rack_cross_set;
  rack_copy(&gen->full_player_rack, &gen->player_rack);
  rack_copy(&gen->bingo_alpha_rack, &gen->player_rack);

  const MachineLetter current_letter =
      gen_cache_get_letter(gen, gen->current_left_col);
  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    shadow_start_nonplaythrough(gen);
  } else {
    shadow_start_playthrough(gen, current_letter);
  }

  gen->rack_cross_set = original_rack_cross_set;
  rack_copy(&gen->player_rack, &gen->full_player_rack);
}

// The algorithm used in this file for
// shadow playing was originally developed in wolges.
// For more details about the shadow playing algorithm, see
// https://github.com/andy-k/wolges/blob/main/details.txt
void shadow_play_for_anchor(MoveGen *gen, int col) {
  // Shadow playing is designed to find the best plays first. When we find plays
  // for endgame using MOVE_RECORD_ALL_SMALL, we need to find all of the plays,
  // and because they are ranked for search in the endgame code rather than
  // here, they're returned unordered.
  //
  // It would be better not to even use these Anchor structs in the first place
  // for MOVE_RECORD_ALL_SMALL (or MOVE_RECORD_ALL and instead to just add moves
  // while looping over the board), but we'll put that off until after other
  // MoveGen changes land.
  if (gen->move_record_type == MOVE_RECORD_ALL_SMALL) {
    anchor_heap_add_unheaped_anchor(&gen->anchor_heap, gen->current_row_index,
                                    col, gen->last_anchor_col, gen->dir,
                                    EQUITY_MAX_VALUE, EQUITY_MAX_VALUE);
    return;
  }

  // Set cols
  gen->current_left_col = col;
  gen->current_right_col = col;

  // Set leftx/rightx
  gen->anchor_left_extension_set = gen_cache_get_left_extension_set(gen, col);
  gen->anchor_right_extension_set = gen_cache_get_right_extension_set(gen, col);

  // Reset unrestricted multipliers
  gen->num_unrestricted_multipliers = 0;
  memset(gen->descending_effective_letter_multipliers, 0,
         sizeof(gen->descending_effective_letter_multipliers));
  gen->last_word_multiplier = 1;

  // Reset available tile scores
  memcpy(gen->descending_tile_scores, gen->full_rack_descending_tile_scores,
         sizeof(gen->descending_tile_scores));
  // Reset score totals
  gen->shadow_mainword_restricted_score = 0;
  gen->shadow_perpendicular_additional_score = 0;
  gen->shadow_word_multiplier = 1;

  // Reset shadow score
  gen->highest_shadow_equity = 0;

  // Set the number of letters
  gen->number_of_letters_on_rack = rack_get_total_letters(&gen->player_rack);

  // Set the current anchor column
  gen->current_anchor_col = col;

  // Reset tiles played
  gen->tiles_played = 0;
  gen->max_tiles_to_play = 0;
  wmp_move_gen_reset_playthrough(&gen->wmp_move_gen);
  wmp_move_gen_reset_anchors(&gen->wmp_move_gen);

  shadow_start(gen);
  if (gen->max_tiles_to_play == 0) {
    return;
  }

  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    wmp_move_gen_add_anchors(&gen->wmp_move_gen, gen->current_row_index, col,
                             gen->last_anchor_col, gen->dir, &gen->anchor_heap);
  } else {
    anchor_heap_add_unheaped_anchor(
        &gen->anchor_heap, gen->current_row_index, col, gen->last_anchor_col,
        gen->dir, gen->highest_shadow_equity, gen->highest_shadow_score);
  }
}

void shadow_by_orientation(MoveGen *gen) {
  for (int row = 0; row < BOARD_DIM; row++) {
    gen->current_row_index = row;
    if (gen->row_number_of_anchors_cache[BOARD_DIM * gen->dir + row] == 0) {
      continue;
    }
    gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
    board_copy_row_cache(gen->lanes_cache, gen->row_cache,
                         gen->current_row_index, gen->dir);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (gen_cache_get_is_anchor(gen, col)) {
        shadow_play_for_anchor(gen, col);

        gen->last_anchor_col = col;
        // The next anchor to search after a playthrough tile should
        // leave a gap of one square so that it will not search backwards
        // into the square adjacent to the playthrough tile.
        if (!gen_cache_is_empty(gen, col)) {
          gen->last_anchor_col++;
        }
      }
    }
  }
}

static inline void set_descending_tile_scores(MoveGen *gen) {
  int i = 0;
  const int ld_size = ld_get_size(&gen->ld);
  for (int j = 0; j < ld_size; j++) {
    const MachineLetter j_score_order = ld_get_score_order(&gen->ld, j);
    for (int k = 0; k < rack_get_letter(&gen->player_rack, j_score_order);
         k++) {
      gen->descending_tile_scores[i] = gen->tile_scores[j_score_order];
      i++;
    }
  }
  memcpy(gen->full_rack_descending_tile_scores, gen->descending_tile_scores,
         sizeof(gen->descending_tile_scores));
}

void gen_load_position(MoveGen *gen, const MoveGenArgs *args) {
  const Game *game = args->game;
  move_record_t move_record_type = args->move_record_type;
  move_sort_t move_sort_type = args->move_sort_type;
  MoveList *move_list = args->move_list;
  const KWG *override_kwg = args->override_kwg;
  gen->eq_margin_movegen = args->eq_margin_movegen;

  gen->board = game_get_board(game);
  gen->player_index = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, gen->player_index);
  const Player *opponent = game_get_player(game, 1 - gen->player_index);

  memcpy(&gen->ld, game_get_ld(game), sizeof(LetterDistribution));
  gen->kwg = player_get_kwg(player);
  gen->kwg = (override_kwg == NULL) ? player_get_kwg(player) : override_kwg;
  gen->klv = player_get_klv(player);
  gen->board_number_of_tiles_played = board_get_tiles_played(gen->board);
  rack_copy(&gen->opponent_rack, player_get_rack(opponent));
  rack_copy(&gen->player_rack, player_get_rack(player));
  rack_set_dist_size(&gen->leave, ld_get_size(&gen->ld));
  wmp_move_gen_init(&gen->wmp_move_gen, &gen->ld, &gen->player_rack,
                    player_get_wmp(player));

  gen->bingo_bonus = game_get_bingo_bonus(game);
  gen->number_of_tiles_in_bag = bag_get_letters(game_get_bag(game));
  gen->kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
  gen->move_sort_type = move_sort_type;
  gen->move_record_type = move_record_type;
  gen->move_list = move_list;
  gen->cross_index =
      board_get_cross_set_index(gen->kwgs_are_shared, gen->player_index);

  // Reset the move list
  if (move_record_type == MOVE_RECORD_ALL_SMALL) {
    small_move_list_reset(gen->move_list);
  } else {
    move_list_reset(gen->move_list);
  }

  // Reset the best and current moves
  gen->best_move_index = 0;
  move_set_equity(gen_get_best_move(gen), EQUITY_INITIAL_VALUE);
  gen->best_move_equity_or_score = EQUITY_INITIAL_VALUE;
  gen->cutoff_equity_or_score = EQUITY_INITIAL_VALUE;

  // Set rack cross set and cache ld's tile scores
  gen->rack_cross_set = 0;
  memset(gen->tile_scores, 0, sizeof(gen->tile_scores));
  for (int i = 0; i < ld_get_size(&gen->ld); i++) {
    if (rack_get_letter(&gen->player_rack, i) > 0) {
      gen->rack_cross_set = gen->rack_cross_set | ((uint64_t)1 << i);
    }
    gen->tile_scores[i] = ld_get_score(&gen->ld, i);
    gen->tile_scores[get_blanked_machine_letter(i)] =
        ld_get_score(&gen->ld, BLANK_MACHINE_LETTER);
  }

  set_descending_tile_scores(gen);

  board_load_number_of_row_anchors_cache(gen->board,
                                         gen->row_number_of_anchors_cache);
  board_load_lanes_cache(gen->board, gen->cross_index, gen->lanes_cache);

  board_copy_opening_penalties(gen->board, gen->opening_move_penalties);

  gen->is_wordsmog = game_get_variant(game) == GAME_VARIANT_WORDSMOG;
}

void gen_look_up_leaves_and_record_exchanges(MoveGen *gen) {
  leave_map_init(&gen->player_rack, &gen->leave_map);
  if (rack_get_total_letters(&gen->player_rack) < RACK_SIZE) {
    leave_map_set_current_value(
        &gen->leave_map, klv_get_leave_value(gen->klv, &gen->player_rack));
  } else {
    leave_map_set_current_value(&gen->leave_map, EQUITY_INITIAL_VALUE);
  }

  for (int i = 0; i < (RACK_SIZE); i++) {
    gen->best_leaves[i] = EQUITY_INITIAL_VALUE;
  }

  if (gen->number_of_tiles_in_bag > 0) {
    // Set the best leaves and maybe add exchanges.
    // gen->leave_map.current_index moves differently when filling
    // leave_values than when reading from it to generate plays. Start at 0,
    // which represents using (exchanging) gen->player_rack->number_of_letters
    // tiles and keeping 0 tiles.
    leave_map_set_current_index(&gen->leave_map, 0);
    uint32_t node_index = kwg_get_dawg_root_node_index(gen->klv->kwg);
    rack_reset(&gen->leave);
    // Assumes the player has drawn a full rack but not the opponent.
    generate_exchange_moves(
        gen, &gen->leave, node_index, 0, 0,
        gen->number_of_tiles_in_bag +
                rack_get_total_letters(&gen->opponent_rack) >=
            (RACK_SIZE * 2));
  }
}

void gen_shadow(MoveGen *gen) {
  // Set the leave_map index to 2^number_of_letters - 1, which represents
  // using (playing) zero tiles and keeping
  // gen->player_rack->number_of_letters tiles.
  leave_map_set_current_index(
      &gen->leave_map, (1 << rack_get_total_letters(&gen->player_rack)) - 1);
  anchor_heap_reset(&gen->anchor_heap);

  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir;
    shadow_by_orientation(gen);
  }

  // Also unnecessary for MOVE_RECORD_ALL, but our tests might care about the
  // ordering of output.
  if (gen->move_record_type != MOVE_RECORD_ALL_SMALL) {
    anchor_heapify_all(&gen->anchor_heap);
  }
}

void gen_record_scoring_plays(MoveGen *gen) {
  // Reset the reused generator fields
  gen->tiles_played = 0;

  // Set these fields to values outside their valid ranges so the row cache gets
  // loaded for the first anchor.
  gen->current_row_index = -1;
  gen->dir = -1;

  const uint32_t kwg_root_node_index = kwg_get_root_node_index(gen->kwg);
  if (gen->is_wordsmog) {
    rack_reset(&gen->full_player_rack);
  }
  while (gen->anchor_heap.count > 0) {
    const Anchor anchor = anchor_heap_extract_max(&gen->anchor_heap);
    if (better_play_has_been_found(gen, anchor.highest_possible_equity)) {
      break;
    }
    gen->current_anchor_col = anchor.col;
    // Don't recopy the row cache if we're working on the same board lane
    // as the previous anchor. When anchors have been sorted by descending
    // max equity, doing this check is a wash, but it helps for endgame when
    // we are just scanning over the board in order.
    if ((gen->current_row_index != anchor.row) || (gen->dir != anchor.dir)) {
      gen->current_row_index = anchor.row;
      gen->dir = anchor.dir;
      board_copy_row_cache(gen->lanes_cache, gen->row_cache, anchor.row,
                           anchor.dir);
    }
    gen->last_anchor_col = anchor.last_anchor_col;
    gen->anchor_right_extension_set =
        gen_cache_get_right_extension_set(gen, gen->current_anchor_col);
    gen->current_anchor_highest_possible_score = anchor.highest_possible_score;
    if (gen->is_wordsmog) {
      recursive_gen_alpha(gen, anchor.col, anchor.col, anchor.col,
                          gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1, 0);
    } else if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
      wordmap_gen(gen, &anchor);
    } else {
      recursive_gen(gen, anchor.col, kwg_root_node_index, anchor.col,
                    anchor.col, gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1,
                    0);
    }

    // If a better play has been found than should have been possible for
    // this anchor, highest_possible_equity was invalid.
    assert(!better_play_has_been_found(gen, anchor.highest_possible_equity));
  }
}

void gen_record_pass(MoveGen *gen) {
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
    move_list_set_spare_move_as_pass(gen->move_list);
    move_list_insert_spare_move(gen->move_list, EQUITY_PASS_VALUE);
    break;
  case MOVE_RECORD_BEST:
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:;
    const Move *top_move = gen_get_readonly_best_move(gen);
    Move *spare_move = move_list_get_spare_move(gen->move_list);
    if (move_get_equity(top_move) < EQUITY_PASS_VALUE) {
      move_list_set_spare_move_as_pass(gen->move_list);
    } else {
      move_copy(spare_move, top_move);
    }
    move_list_insert_spare_move_top_equity(gen->move_list,
                                           move_get_equity(spare_move));
    break;
  case MOVE_RECORD_ALL_SMALL:
    move_list_set_spare_small_move_as_pass(gen->move_list);
    move_list_insert_spare_small_move(gen->move_list);
    break;
  }
}

void generate_moves(const MoveGenArgs *args) {
  MoveGen *gen = get_movegen(args->thread_index);
  gen_load_position(gen, args);
  gen_look_up_leaves_and_record_exchanges(gen);

  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    wmp_move_gen_check_nonplaythrough_existence(
        &gen->wmp_move_gen, gen->number_of_tiles_in_bag > 0, &gen->leave_map);
  }

  gen_shadow(gen);
  gen_record_scoring_plays(gen);
  gen_record_pass(gen);
}
