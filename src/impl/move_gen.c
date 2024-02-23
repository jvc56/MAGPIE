#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"

#include "../ent/anchor.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/leave_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"

#include "../util/util.h"

#define INITIAL_LAST_ANCHOR_COL (BOARD_DIM)

typedef struct MoveGen {
  // Owned by this MoveGen struct
  int current_row_index;
  int current_anchor_col;
  int last_anchor_col;
  int dir;
  int max_tiles_to_play;
  int tiles_played;
  int number_of_plays;
  int move_sort_type;
  int move_record_type;
  int number_of_tiles_in_bag;
  int player_index;
  bool kwgs_are_shared;
  bool transposed;

  uint8_t row_letter_cache[(BOARD_DIM)];
  bool is_cross_word_cache[(BOARD_DIM)];
  uint8_t bonus_square_cache[(BOARD_DIM)];
  uint64_t cross_set_cache[(BOARD_DIM)];
  uint64_t hz_cross_set_cache[(BOARD_DIM)];
  uint8_t cross_score_cache[(BOARD_DIM)];
  bool is_anchorless_row_cache[(BOARD_DIM) * 2];
  bool is_anchor_cache[(BOARD_DIM) * (BOARD_DIM) * 2];

  int bag_tiles_remaining;

  uint8_t strip[(BOARD_DIM)];
  uint8_t *exchange_strip;
  LeaveMap *leave_map;
  // Shadow plays
  int current_left_col;
  int current_right_col;
  double highest_shadow_equity;
  uint64_t rack_cross_set;
  int number_of_letters_on_rack;
  int descending_tile_scores[(RACK_SIZE)];
  double best_leaves[(RACK_SIZE)];
  AnchorList *anchor_list;
  uint8_t tile_scores[(MAX_ALPHABET_SIZE)];

  // Owned by the caller
  const LetterDistribution *ld;
  const KLV *klv;
  const KWG *kwg;
  const Rack *opponent_rack;
  const Board *board;
  // Player rack is modified when generating exchanges
  Rack *player_rack;
  // Output owned by this MoveGen struct
  MoveList *move_list;
} MoveGen;

// Cache move generators since destroying
// and recreating a movegen for
// every request to generate moves would
// be expensive. The infer and sim functions
// don't have this problem since they are
// only called once per command.
static MoveGen *cached_gens[MAX_THREADS];

MoveGen *create_generator(int ld_size) {
  MoveGen *generator = malloc_or_die(sizeof(MoveGen));
  generator->anchor_list = anchor_list_create();
  generator->leave_map = leave_map_create(ld_size);
  generator->player_rack = rack_create(ld_size);
  generator->tiles_played = 0;
  generator->dir = BOARD_HORIZONTAL_DIRECTION;
  generator->exchange_strip =
      (uint8_t *)malloc_or_die(ld_size * sizeof(uint8_t));
  return generator;
}

void destroy_generator(MoveGen *gen) {
  if (!gen) {
    return;
  }
  anchor_list_destroy(gen->anchor_list);
  leave_map_destroy(gen->leave_map);
  rack_destroy(gen->player_rack);
  free(gen->exchange_strip);
  free(gen);
}

MoveGen *get_movegen(int thread_index, int ld_size) {
  if (!cached_gens[thread_index]) {
    cached_gens[thread_index] = create_generator(ld_size);
  }
  return cached_gens[thread_index];
}

void gen_destroy_cache() {
  for (int i = 0; i < (MAX_THREADS); i++) {
    destroy_generator(cached_gens[i]);
    cached_gens[i] = NULL;
  }
}

void go_on(MoveGen *gen, int current_col, uint8_t L, uint32_t new_node_index,
           bool accepts, int leftstrip, int rightstrip, bool unique_play,
           int main_word_score, int word_multiplier, int cross_score);

// This function is only used for testing and is exposed
// in the move_gen_pi.h header in the test directory.
AnchorList *gen_get_anchor_list(int thread_index) {
  return cached_gens[thread_index]->anchor_list;
}

static inline void load_is_cross_word_cache(MoveGen *gen, int row) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->is_cross_word_cache[col] =
        (row > 0 &&
         !board_is_empty(gen->board, row - 1, col, gen->transposed)) ||
        ((row < BOARD_DIM - 1) &&
         !board_is_empty(gen->board, row + 1, col, gen->transposed));
  }
}

static inline int get_anchorless_row_index(int row, int dir) {
  return dir * BOARD_DIM + row;
}

static inline void load_is_anchor_cache(MoveGen *gen) {
  for (int dir = 0; dir < 2; dir++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      const int anchorless_row_index = get_anchorless_row_index(row, dir);
      gen->is_anchorless_row_cache[anchorless_row_index] = true;
      for (int col = 0; col < BOARD_DIM; col++) {
        const bool is_anchor =
            board_get_anchor(gen->board, row, col, dir, gen->transposed);
        gen->is_anchor_cache[board_get_tindex_dir(row, col, dir,
                                                  gen->transposed)] = is_anchor;
        if (is_anchor) {
          gen->is_anchorless_row_cache[anchorless_row_index] = false;
        }
      }
    }
    gen->transposed = !gen->transposed;
  }
}

static inline bool get_is_anchor_cache(MoveGen *gen, int row, int col,
                                       int dir) {
  return gen
      ->is_anchor_cache[board_get_tindex_dir(row, col, dir, gen->transposed)];
}

static inline bool cached_is_cross_word(MoveGen *gen, int col) {
  return gen->is_cross_word_cache[col];
}

static inline void load_bonus_square_cache(MoveGen *gen, int row) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->bonus_square_cache[col] =
        board_get_bonus_square(gen->board, row, col, gen->transposed);
  }
}

static inline uint8_t get_bonus_square_cache(const MoveGen *gen, int col) {
  return gen->bonus_square_cache[col];
}

static inline void load_cross_set_cache(MoveGen *gen, int row, int cs_dir,
                                        int cross_set_index) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->cross_set_cache[col] = board_get_cross_set(
        gen->board, row, col, cs_dir, cross_set_index, gen->transposed);
  }
}

static inline uint64_t get_cross_set_cache(const MoveGen *gen, int col) {
  return gen->cross_set_cache[col];
}

static inline void load_hz_cross_set_cache(MoveGen *gen, int row,
                                           int cross_set_index) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->hz_cross_set_cache[col] =
        board_get_cross_set(gen->board, row, col, BOARD_HORIZONTAL_DIRECTION,
                            cross_set_index, gen->transposed);
  }
}

static inline uint64_t get_hz_cross_set_cache(const MoveGen *gen, int col) {
  return gen->hz_cross_set_cache[col];
}

static inline void load_cross_score_cache(MoveGen *gen, int row, int cs_dir,
                                          int cross_set_index) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->cross_score_cache[col] = board_get_cross_score(
        gen->board, row, col, cs_dir, cross_set_index, gen->transposed);
  }
}

static inline uint8_t get_cross_score_cache(const MoveGen *gen, int col) {
  return gen->cross_score_cache[col];
}

double get_static_equity(MoveGen *gen) {
  return static_eval_get_move_equity_with_leave_value(
      gen->ld, move_list_get_spare_move(gen->move_list), gen->board,
      gen->player_rack, gen->opponent_rack, gen->number_of_tiles_in_bag,
      leave_map_get_current_value(gen->leave_map));
}

void record_play(MoveGen *gen, int leftstrip, int rightstrip,
                 game_event_t move_type, int main_word_score,
                 int word_multiplier, int cross_score) {
  int start_row = gen->current_row_index;
  int start_col = leftstrip;
  int tiles_played = gen->tiles_played;

  int score = 0;

  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {

    int bingo_bonus = 0;
    if (tiles_played == RACK_SIZE) {
      bingo_bonus = DEFAULT_BINGO_BONUS;
    }

    score = main_word_score * word_multiplier + cross_score + bingo_bonus;

    Move *spare_move = move_list_get_spare_move(gen->move_list);
    move_set_all_except_equity(spare_move, gen->strip, leftstrip, rightstrip,
                               score, start_row, start_col, tiles_played,
                               gen->dir, move_type);
    if (board_is_dir_vertical(gen->dir)) {
      move_set_row_start(spare_move, start_col);
      move_set_col_start(spare_move, start_row);
    }
  } else if (move_type == GAME_EVENT_EXCHANGE) {
    // ignore the empty exchange case
    if (rightstrip == 0) {
      return;
    }
    move_list_set_spare_move(gen->move_list, gen->exchange_strip, leftstrip,
                             rightstrip, score, start_row, start_col,
                             rightstrip, gen->dir, move_type);
  }

  if (gen->move_record_type == MOVE_RECORD_ALL) {
    double equity;
    if (gen->move_sort_type == MOVE_SORT_EQUITY) {
      equity = get_static_equity(gen);
    } else {
      equity = score;
    }
    move_list_insert_spare_move(gen->move_list, equity);
  } else {
    move_list_insert_spare_move_top_equity(gen->move_list,
                                           get_static_equity(gen));
  }
}

void generate_exchange_moves(MoveGen *gen, uint8_t ml, int stripidx,
                             bool add_exchange) {
  const uint32_t ld_size = ld_get_size(gen->ld);
  while (ml < ld_size && rack_get_letter(gen->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    // The recording of an exchange should never require
    // the opponent's rack.

    // Ignore the empty exchange case for full racks
    // to avoid out of bounds errors for the best_leaves array
    int number_of_letters_on_rack = rack_get_total_letters(gen->player_rack);
    if (number_of_letters_on_rack < RACK_SIZE) {
      double current_value = klv_get_leave_value(gen->klv, gen->player_rack);
      leave_map_set_current_value(gen->leave_map, current_value);
      if (current_value > gen->best_leaves[number_of_letters_on_rack]) {
        gen->best_leaves[number_of_letters_on_rack] = current_value;
      }
      if (add_exchange) {
        record_play(gen, 0, stripidx, GAME_EVENT_EXCHANGE, 0, 0, 0);
      }
    }
  } else {
    generate_exchange_moves(gen, ml + 1, stripidx, add_exchange);
    int num_this = rack_get_letter(gen->player_rack, ml);
    for (int i = 0; i < num_this; i++) {
      gen->exchange_strip[stripidx] = ml;
      stripidx += 1;
      leave_map_take_letter_and_update_current_index(gen->leave_map,
                                                     gen->player_rack, ml);
      generate_exchange_moves(gen, ml + 1, stripidx, add_exchange);
    }
    for (int i = 0; i < num_this; i++) {
      leave_map_add_letter_and_update_current_index(gen->leave_map,
                                                    gen->player_rack, ml);
    }
  }
}

static inline void load_row_letter_cache(MoveGen *gen, int row) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->row_letter_cache[col] =
        board_get_letter(gen->board, row, col, gen->transposed);
  }
}

static inline uint8_t get_letter_cache(const MoveGen *gen, int col) {
  return gen->row_letter_cache[col];
}

static inline int is_empty_cache(const MoveGen *gen, int col) {
  return get_letter_cache(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static inline bool better_play_has_been_found(const MoveGen *gen,
                                              double highest_possible_value) {
  Move *move = move_list_get_move(gen->move_list, 0);
  const double best_value_found = (gen->move_sort_type == MOVE_SORT_EQUITY)
                                      ? move_get_equity(move)
                                      : move_get_score(move);
  return highest_possible_value + COMPARE_MOVES_EPSILON <= best_value_found;
}

void recursive_gen(MoveGen *gen, int col, uint32_t node_index, int leftstrip,
                   int rightstrip, bool unique_play, int main_word_score,
                   int word_multiplier, int cross_score) {
  const uint8_t current_letter = get_letter_cache(gen, col);
  const uint64_t cross_set = get_cross_set_cache(gen, col);
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    int raw = get_unblanked_machine_letter(current_letter);
    int next_node_index = 0;
    bool accepts = false;
    for (int i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      if (kwg_node_tile(node) == raw) {
        next_node_index = kwg_node_arc_index(node);
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
  } else if (!rack_is_empty(gen->player_rack)) {
    for (int i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      int ml = kwg_node_tile(node);
      int number_of_ml = rack_get_letter(gen->player_rack, ml);
      if (ml != 0 &&
          (number_of_ml != 0 ||
           rack_get_letter(gen->player_rack, BLANK_MACHINE_LETTER) != 0) &&
          board_is_letter_allowed_in_cross_set(cross_set, ml)) {
        int next_node_index = kwg_node_arc_index(node);
        bool accepts = kwg_node_accepts(node);
        if (number_of_ml > 0) {
          leave_map_take_letter_and_update_current_index(gen->leave_map,
                                                         gen->player_rack, ml);
          gen->tiles_played++;
          go_on(gen, col, ml, next_node_index, accepts, leftstrip, rightstrip,
                unique_play, main_word_score, word_multiplier, cross_score);
          gen->tiles_played--;
          leave_map_add_letter_and_update_current_index(gen->leave_map,
                                                        gen->player_rack, ml);
        }
        // check blank
        if (rack_get_letter(gen->player_rack, BLANK_MACHINE_LETTER) > 0) {
          leave_map_take_letter_and_update_current_index(
              gen->leave_map, gen->player_rack, BLANK_MACHINE_LETTER);
          gen->tiles_played++;
          go_on(gen, col, get_blanked_machine_letter(ml), next_node_index,
                accepts, leftstrip, rightstrip, unique_play, main_word_score,
                word_multiplier, cross_score);
          gen->tiles_played--;
          leave_map_add_letter_and_update_current_index(
              gen->leave_map, gen->player_rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
}

void go_on(MoveGen *gen, int current_col, uint8_t L, uint32_t new_node_index,
           bool accepts, int leftstrip, int rightstrip, bool unique_play,
           int main_word_score, int word_multiplier, int cross_score) {
  // Handle incremental scoring
  const uint8_t bonus_square = gen->bonus_square_cache[current_col];
  int letter_multiplier = 1;
  int this_word_multiplier = 1;
  bool fresh_tile = false;

  const bool square_is_empty = is_empty_cache(gen, current_col);
  uint8_t ml;
  if (!square_is_empty) {
    gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    ml = gen->row_letter_cache[current_col];
  } else {
    gen->strip[current_col] = L;
    ml = L;
    fresh_tile = true;
    this_word_multiplier = bonus_square >> 4;
    letter_multiplier = bonus_square & 0x0F;
  }

  int inc_word_multiplier = this_word_multiplier * word_multiplier;

  int lsm = 0;

  if (!get_is_blanked(ml)) {
    lsm = ld_get_score(gen->ld, ml) * letter_multiplier;
  }

  int inc_main_word_score = lsm + main_word_score;

  int inc_cross_scores = cross_score;

  if (fresh_tile && gen->is_cross_word_cache[current_col]) {
    inc_cross_scores +=
        (lsm + gen->cross_score_cache[current_col]) * this_word_multiplier;
  }

  if (current_col <= gen->current_anchor_col) {
    if (square_is_empty && gen->dir &&
        get_cross_set_cache(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || is_empty_cache(gen, current_col - 1);

    if (accepts && no_letter_directly_left &&
        gen->tiles_played > !unique_play) {
      record_play(gen, leftstrip, rightstrip, GAME_EVENT_TILE_PLACEMENT_MOVE,
                  inc_main_word_score, inc_word_multiplier, inc_cross_scores);
    }

    if (new_node_index == 0) {
      return;
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen(gen, current_col - 1, new_node_index, leftstrip, rightstrip,
                    unique_play, inc_main_word_score, inc_word_multiplier,
                    inc_cross_scores);
    }

    uint32_t separation_node_index = kwg_get_next_node_index(
        gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        gen->current_anchor_col < BOARD_DIM - 1) {
      recursive_gen(gen, gen->current_anchor_col + 1, separation_node_index,
                    leftstrip, rightstrip, unique_play, inc_main_word_score,
                    inc_word_multiplier, inc_cross_scores);
    }
  } else {
    if (square_is_empty && !unique_play && gen->dir &&
        get_hz_cross_set_cache(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    rightstrip = current_col;
    bool no_letter_directly_right =
        (current_col == BOARD_DIM - 1) || is_empty_cache(gen, current_col + 1);

    if (accepts && no_letter_directly_right &&
        gen->tiles_played > !unique_play) {
      record_play(gen, leftstrip, rightstrip, GAME_EVENT_TILE_PLACEMENT_MOVE,
                  inc_main_word_score, inc_word_multiplier, inc_cross_scores);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen(gen, current_col + 1, new_node_index, leftstrip, rightstrip,
                    unique_play, inc_main_word_score, inc_word_multiplier,
                    inc_cross_scores);
    }
  }
}

static inline bool
shadow_board_is_letter_allowed_in_cross_set(const MoveGen *gen, int col) {
  uint64_t cross_set = get_cross_set_cache(gen, col);
  // board_is_letter_allowed_in_cross_set if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  return (cross_set & gen->rack_cross_set) != 0 ||
         ((gen->rack_cross_set & 1) && cross_set);
}

void shadow_record(MoveGen *gen, int left_col, int right_col,
                   int main_played_through_score,
                   int perpendicular_additional_score, int word_multiplier) {
  int sorted_effective_letter_multipliers[(RACK_SIZE)];
  int current_tiles_played = 0;
  for (int current_col = left_col; current_col <= right_col; current_col++) {
    const uint8_t current_letter = get_letter_cache(gen, current_col);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      const uint8_t bonus_square = get_bonus_square_cache(gen, current_col);
      int this_word_multiplier = bonus_square >> 4;
      int letter_multiplier = bonus_square & 0x0F;
      bool is_cross_word = cached_is_cross_word(gen, current_col);
      int effective_letter_multiplier =
          letter_multiplier *
          ((this_word_multiplier * is_cross_word) + word_multiplier);
      // Insert the effective multiplier.
      int insert_index = current_tiles_played;
      for (; insert_index > 0 &&
             sorted_effective_letter_multipliers[insert_index - 1] <
                 effective_letter_multiplier;
           insert_index--) {
        sorted_effective_letter_multipliers[insert_index] =
            sorted_effective_letter_multipliers[insert_index - 1];
      }
      sorted_effective_letter_multipliers[insert_index] =
          effective_letter_multiplier;
      current_tiles_played++;
    }
  }

  int tiles_played_score = 0;
  for (int i = 0; i < current_tiles_played; i++) {
    tiles_played_score +=
        gen->descending_tile_scores[i] * sorted_effective_letter_multipliers[i];
  }

  int bingo_bonus = 0;
  if (gen->tiles_played == RACK_SIZE) {
    bingo_bonus = DEFAULT_BINGO_BONUS;
  }

  int score = tiles_played_score +
              (main_played_through_score * word_multiplier) +
              perpendicular_additional_score + bingo_bonus;
  double equity = (double)score;
  if (gen->move_sort_type == MOVE_SORT_EQUITY) {
    equity += static_eval_get_shadow_equity(
        gen->ld, gen->opponent_rack, gen->best_leaves,
        gen->descending_tile_scores, gen->number_of_tiles_in_bag,
        gen->number_of_letters_on_rack, gen->tiles_played);
  }
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
  if (gen->tiles_played > gen->max_tiles_to_play) {
    gen->max_tiles_to_play = gen->tiles_played;
  }
}

void shadow_play_right(MoveGen *gen, int main_played_through_score,
                       int perpendicular_additional_score, int word_multiplier,
                       bool is_unique) {
  if (gen->current_right_col == (BOARD_DIM - 1) ||
      gen->tiles_played >= gen->number_of_letters_on_rack) {
    // We have gone all the way left or right.
    return;
  }

  int original_current_right_col = gen->current_right_col;
  gen->current_right_col++;
  gen->tiles_played++;

  const uint64_t cross_set = get_cross_set_cache(gen, gen->current_right_col);
  // board_is_letter_allowed_in_cross_set if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    const uint8_t bonus_square =
        get_bonus_square_cache(gen, gen->current_right_col);
    const int cross_score = get_cross_score_cache(gen, gen->current_right_col);
    int this_word_multiplier = bonus_square >> 4;
    perpendicular_additional_score += cross_score * this_word_multiplier;
    word_multiplier *= this_word_multiplier;

    if (cross_set == TRIVIAL_CROSS_SET) {
      // If the horizontal direction is the trivial cross-set, this means
      // that there are no letters perpendicular to where we just placed
      // this letter. So any play we generate here should be unique.
      // We use this to avoid generating duplicates of single-tile plays.
      is_unique = true;
    }
    // Continue playing right until an empty square or the edge of board is
    // hit
    while (gen->current_right_col + 1 < BOARD_DIM) {
      uint8_t next_letter = get_letter_cache(gen, gen->current_right_col + 1);
      if (next_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        break;
      }
      if (!get_is_blanked(next_letter)) {
        main_played_through_score += gen->tile_scores[next_letter];
      }
      gen->current_right_col++;
    }

    if (gen->tiles_played + is_unique >= 2) {
      shadow_record(gen, gen->current_left_col, gen->current_right_col,
                    main_played_through_score, perpendicular_additional_score,
                    word_multiplier);
    }

    shadow_play_right(gen, main_played_through_score,
                      perpendicular_additional_score, word_multiplier,
                      is_unique);
  }
  gen->tiles_played--;
  gen->current_right_col = original_current_right_col;
}

void shadow_play_left(MoveGen *gen, int main_played_through_score,
                      int perpendicular_additional_score, int word_multiplier,
                      bool is_unique) {
  // Go left until hitting an empty square or the edge of the board.

  if (gen->current_left_col == 0 ||
      gen->current_left_col == gen->last_anchor_col + 1 ||
      gen->tiles_played >= gen->number_of_letters_on_rack) {
    // We have gone all the way left or right.
    return;
  }

  int original_current_left_col = gen->current_left_col;
  gen->current_left_col--;
  gen->tiles_played++;
  const uint64_t cross_set = get_cross_set_cache(gen, gen->current_left_col);
  // board_is_letter_allowed_in_cross_set if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    const uint8_t bonus_square =
        get_bonus_square_cache(gen, gen->current_left_col);
    const int cross_score = get_cross_score_cache(gen, gen->current_left_col);
    int this_word_multiplier = bonus_square >> 4;
    perpendicular_additional_score += cross_score * this_word_multiplier;
    word_multiplier *= this_word_multiplier;

    if (cross_set == TRIVIAL_CROSS_SET) {
      // See equivalent in shadow_play_right for the reasoning here.
      is_unique = true;
    }

    if (gen->tiles_played + is_unique >= 2) {
      shadow_record(gen, gen->current_left_col, gen->current_right_col,
                    main_played_through_score, perpendicular_additional_score,
                    word_multiplier);
    }
    shadow_play_left(gen, main_played_through_score,
                     perpendicular_additional_score, word_multiplier,
                     is_unique);
  }
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier, is_unique);
  gen->current_left_col = original_current_left_col;
  gen->tiles_played--;
}

void shadow_start(MoveGen *gen) {
  int main_played_through_score = 0;
  int perpendicular_additional_score = 0;
  int word_multiplier = 1;
  uint8_t current_letter = get_letter_cache(gen, gen->current_left_col);

  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    // Only play a letter if a letter from the rack fits in the cross set
    if (shadow_board_is_letter_allowed_in_cross_set(gen,
                                                    gen->current_left_col)) {
      // Play tile and update scoring parameters

      const uint8_t bonus_square =
          get_bonus_square_cache(gen, gen->current_left_col);
      const int cross_score = get_cross_score_cache(gen, gen->current_left_col);
      int this_word_multiplier = bonus_square >> 4;
      perpendicular_additional_score += (cross_score * this_word_multiplier);
      word_multiplier = this_word_multiplier;
      gen->tiles_played++;
      if (!board_is_dir_vertical(gen->dir)) {
        // word_multiplier is always hard-coded as 0 since we are recording a
        // single tile
        shadow_record(gen, gen->current_left_col, gen->current_right_col,
                      main_played_through_score, perpendicular_additional_score,
                      0);
      }
    } else {
      // Nothing hooks here, return
      return;
    }
  } else {
    // Traverse the full length of the tiles on the board until hitting an
    // empty square
    while (1) {
      if (!get_is_blanked(current_letter)) {
        main_played_through_score += gen->tile_scores[current_letter];
      }
      if (gen->current_left_col == 0 ||
          gen->current_left_col == gen->last_anchor_col + 1) {
        break;
      }
      gen->current_left_col--;
      current_letter = get_letter_cache(gen, gen->current_left_col);
      if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        gen->current_left_col++;
        break;
      }
    }
  }
  shadow_play_left(gen, main_played_through_score,
                   perpendicular_additional_score, word_multiplier,
                   !board_is_dir_vertical(gen->dir));
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier,
                    !board_is_dir_vertical(gen->dir));
}

// The algorithm used in this file for
// shadow playing was originally developed in wolges.
// For more details about the shadow playing algorithm, see
// https://github.com/andy-k/wolges/blob/main/details.txt
void shadow_play_for_anchor(MoveGen *gen, int col) {
  // set cols
  gen->current_left_col = col;
  gen->current_right_col = col;

  // Reset shadow score
  gen->highest_shadow_equity = 0;

  // Set the number of letters
  gen->number_of_letters_on_rack = rack_get_total_letters(gen->player_rack);

  // Set the current anchor column
  gen->current_anchor_col = col;

  // Reset tiles played
  gen->tiles_played = 0;
  gen->max_tiles_to_play = 0;

  shadow_start(gen);
  if (gen->max_tiles_to_play == 0) {
    return;
  }
  anchor_list_add_anchor(gen->anchor_list, gen->current_row_index, col,
                         gen->last_anchor_col, gen->transposed,
                         board_is_dir_vertical(gen->dir),
                         gen->highest_shadow_equity);
}

void shadow_by_orientation(MoveGen *gen, int dir) {
  for (int row = 0; row < BOARD_DIM; row++) {
    gen->current_row_index = row;
    const int anchorless_row_index = get_anchorless_row_index(row, dir);
    if (gen->is_anchorless_row_cache[anchorless_row_index]) {
      continue;
    }
    gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
    load_row_letter_cache(gen, gen->current_row_index);
    load_is_cross_word_cache(gen, gen->current_row_index);
    load_bonus_square_cache(gen, gen->current_row_index);
    load_cross_set_cache(
        gen, gen->current_row_index, !board_is_dir_vertical(gen->dir),
        board_get_cross_set_index(gen->kwgs_are_shared, gen->player_index));
    load_cross_score_cache(
        gen, gen->current_row_index, !board_is_dir_vertical(gen->dir),
        board_get_cross_set_index(gen->kwgs_are_shared, gen->player_index));
    for (int col = 0; col < BOARD_DIM; col++) {
      if (get_is_anchor_cache(gen, row, col, dir)) {
        shadow_play_for_anchor(gen, col);
        gen->last_anchor_col = col;
        // The next anchor to search after a playthrough tile should
        // leave a gap of one square so that it will not search backwards
        // into the square adjacent to the playthrough tile.
        if (!is_empty_cache(gen, col)) {
          gen->last_anchor_col++;
        }
      }
    }
  }
}

static inline void set_descending_tile_scores(MoveGen *gen) {
  int i = 0;
  for (int j = 0; j < (int)ld_get_size(gen->ld); j++) {
    int j_score_order = ld_get_score_order(gen->ld, j);
    for (int k = 0; k < rack_get_letter(gen->player_rack, j_score_order); k++) {
      gen->descending_tile_scores[i] = gen->tile_scores[j_score_order];
      i++;
    }
  }
}

void generate_moves(const Game *game, move_record_t move_record_type,
                    move_sort_t move_sort_type, int thread_index,
                    MoveList *move_list) {
  const LetterDistribution *ld = game_get_ld(game);
  MoveGen *gen = get_movegen(thread_index, ld_get_size(ld));
  int player_on_turn_index = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, player_on_turn_index);
  const Player *opponent = game_get_player(game, 1 - player_on_turn_index);

  gen->ld = ld;
  gen->kwg = player_get_kwg(player);
  gen->klv = player_get_klv(player);
  gen->opponent_rack = player_get_rack(opponent);
  gen->board = game_get_board(game);
  gen->player_index = player_on_turn_index;
  rack_copy(gen->player_rack, player_get_rack(player));

  gen->number_of_tiles_in_bag = bag_get_tiles(game_get_bag(game));
  gen->kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
  gen->move_sort_type = move_sort_type;
  gen->move_record_type = move_record_type;
  gen->move_list = move_list;
  gen->transposed = false;

  // Reset the move list
  move_list_reset(gen->move_list);

  leave_map_init(gen->player_rack, gen->leave_map);
  if (rack_get_total_letters(gen->player_rack) < RACK_SIZE) {
    leave_map_set_current_value(
        gen->leave_map, klv_get_leave_value(gen->klv, gen->player_rack));
  } else {
    leave_map_set_current_value(gen->leave_map, INITIAL_TOP_MOVE_EQUITY);
  }

  for (int i = 0; i < (RACK_SIZE); i++) {
    gen->best_leaves[i] = (double)(INITIAL_TOP_MOVE_EQUITY);
  }

  // Set the best leaves and maybe add exchanges.
  generate_exchange_moves(gen, 0, 0, gen->number_of_tiles_in_bag >= RACK_SIZE);

  anchor_list_reset(gen->anchor_list);

  // Set rack cross set and cache ld's tile scores
  gen->rack_cross_set = 0;
  for (int i = 0; i < ld_get_size(gen->ld); i++) {
    if (rack_get_letter(gen->player_rack, i) > 0) {
      gen->rack_cross_set = gen->rack_cross_set | ((uint64_t)1 << i);
    }
    gen->tile_scores[i] = ld_get_score(gen->ld, i);
  }

  set_descending_tile_scores(gen);

  load_is_anchor_cache(gen);

  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir % 2 != 0;
    shadow_by_orientation(gen, dir);
    gen->transposed = !gen->transposed;
  }

  // Reset the reused generator fields
  gen->tiles_played = 0;

  anchor_list_sort(gen->anchor_list);
  const AnchorList *anchor_list = gen->anchor_list;
  const int kwg_root_node_index = kwg_get_root_node_index(gen->kwg);
  for (int i = 0; i < anchor_list_get_count(anchor_list); i++) {
    double anchor_highest_possible_equity =
        anchor_get_highest_possible_equity(anchor_list, i);
    if (gen->move_record_type == MOVE_RECORD_BEST &&
        better_play_has_been_found(gen, anchor_highest_possible_equity)) {
      break;
    }
    gen->current_anchor_col = anchor_get_col(anchor_list, i);
    gen->current_row_index = anchor_get_row(anchor_list, i);
    gen->last_anchor_col = anchor_get_last_anchor_col(anchor_list, i);
    gen->dir = anchor_get_dir(anchor_list, i);
    gen->transposed = anchor_get_transposed(anchor_list, i);
    load_row_letter_cache(gen, gen->current_row_index);
    load_bonus_square_cache(gen, gen->current_row_index);
    load_is_cross_word_cache(gen, gen->current_row_index);
    load_cross_score_cache(
        gen, gen->current_row_index, !board_is_dir_vertical(gen->dir),
        board_get_cross_set_index(gen->kwgs_are_shared, gen->player_index));
    load_cross_set_cache(
        gen, gen->current_row_index, !board_is_dir_vertical(gen->dir),
        board_get_cross_set_index(gen->kwgs_are_shared, gen->player_index));
    load_hz_cross_set_cache(
        gen, gen->current_row_index,
        board_get_cross_set_index(gen->kwgs_are_shared, gen->player_index));
    recursive_gen(gen, gen->current_anchor_col, kwg_root_node_index,
                  gen->current_anchor_col, gen->current_anchor_col,
                  gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1, 0);

    if (gen->move_record_type == MOVE_RECORD_BEST) {
      // If a better play has been found than should have been possible for
      // this anchor, highest_possible_equity was invalid.
      assert(!better_play_has_been_found(gen, anchor_highest_possible_equity));
    }
  }

  Move *top_move = move_list_get_move(gen->move_list, 0);
  // Add the pass move
  if (gen->move_record_type == MOVE_RECORD_ALL ||
      move_get_equity(top_move) < PASS_MOVE_EQUITY) {
    move_list_set_spare_move_as_pass(gen->move_list);
    move_list_insert_spare_move(gen->move_list, PASS_MOVE_EQUITY);
  }
}
