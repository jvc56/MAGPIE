#include "peg_world.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int peg_world_static_pool(const Game *game, MoveList *pool, Move *candidates,
                          int *pass_rank) {
  move_list_reset(pool);
  const MoveGenArgs args = {.game = game,
                            .move_list = pool,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  move_list_sort_moves(pool);
  *pass_rank = -1;
  int count = 0;
  for (int rank = 0; rank < move_list_get_count(pool); rank++) {
    const Move *move = move_list_get_move(pool, rank);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      *pass_rank = rank;
    } else if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      // Exchanges are never candidates; they are legal only with a full
      // rack's worth of tiles in the bag.
      if (bag_get_letters(game_get_bag(game)) < RACK_SIZE) {
        log_fatal("peg1 unexpected exchange");
      }
    } else if (count < PEG_STATIC_POOL_SIZE) {
      move_copy(&candidates[count++], move);
    }
  }
  return count;
}

void peg_world_from_public_counts(const Game *root, const int *unseen, int tile,
                                  Game *scenario) {
  // The racks and bag are set directly rather than by returning the hidden
  // rack to the bag and redrawing: a bag laid out by bag_set_to_tiles (PEG
  // scenarios, 2PEG worlds) cannot take a returned tile on player 1's side.
  const int mover = game_get_player_on_turn_index(root);
  const int letters = ld_get_size(game_get_ld(root));
  game_copy(scenario, root);
  Rack *opponent = player_get_rack(game_get_player(scenario, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < letters; ml++) {
    for (int copy = 0; copy < unseen[ml] - (ml == tile); copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  const MachineLetter last = (MachineLetter)tile;
  bag_set_to_tiles(game_get_bag(scenario), &last, 1);
  if (unseen[tile] < 1 || rack_get_total_letters(opponent) != RACK_SIZE) {
    log_fatal("peg1 invalid public scenario");
  }
}

int peg_world_public_unseen(const Game *game, int *unseen) {
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *own = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    unseen[ml] = ld_get_dist(ld, (MachineLetter)ml) -
                 rack_get_letter(own, (MachineLetter)ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter ml = board_get_letter(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      unseen[get_is_blanked(ml) ? BLANK_MACHINE_LETTER
                                : get_unblanked_machine_letter(ml)]--;
    }
  }
  int total = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (unseen[ml] < 0) {
      log_fatal("1PEG negative public unseen count");
    }
    total += unseen[ml];
  }
  return total;
}

uint64_t peg_world_lane_influence(const Board *board, const Move *move) {
  static const int deltas[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  uint64_t touched = 0;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = vertical ? row_start + tile_idx : row_start;
    const int col = vertical ? col_start : col_start + tile_idx;
    touched |= (UINT64_C(1) << row) | (UINT64_C(1) << (BOARD_DIM + col));
    for (int delta_idx = 0; delta_idx < 4; delta_idx++) {
      int walk_row = row + deltas[delta_idx][0];
      int walk_col = col + deltas[delta_idx][1];
      while (board_is_position_in_bounds(walk_row, walk_col) &&
             board_is_nonempty_or_bricked(board, walk_row, walk_col)) {
        walk_row += deltas[delta_idx][0];
        walk_col += deltas[delta_idx][1];
      }
      if (board_is_position_in_bounds(walk_row, walk_col)) {
        touched |=
            (UINT64_C(1) << walk_row) | (UINT64_C(1) << (BOARD_DIM + walk_col));
      }
    }
  }
  return touched;
}

int peg_world_move_lane(const Move *move) {
  return board_is_dir_vertical(move_get_dir(move))
             ? BOARD_DIM + move_get_col_start(move)
             : move_get_row_start(move);
}

void peg_world_best_scoring_in_lanes(const Game *game, MoveList *list,
                                     uint64_t lanes, const Move *seed,
                                     Move *best) {
  const MoveGenArgs args = {.game = game,
                            .move_list = list,
                            .move_record_type = MOVE_RECORD_BEST,
                            .move_sort_type = MOVE_SORT_SCORE,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .lane_mask = lanes,
                            .initial_best_move = seed,
                            .disable_pat = true};
  generate_moves(&args);
  move_copy(best, move_list_get_move(list, 0));
}

void peg_world_after_candidate(const Game *root, const int *unseen,
                               const Move *cand, const MachineLetter *bag2,
                               int n_drawn, Game *world) {
  const int mover = game_get_player_on_turn_index(root);
  game_copy(world, root);
  int rest[VALUE_NET_TILE_TYPES];
  memcpy(rest, unseen, sizeof(rest));
  rest[bag2[0]]--;
  rest[bag2[1]]--;
  Rack *opponent = player_get_rack(game_get_player(world, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (rest[ml] < 0) {
      log_fatal("peg2 world draws an unavailable tile");
    }
    for (int copy = 0; copy < rest[ml]; copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  play_move_without_drawing_tiles(cand, world);
  Rack *own = player_get_rack(game_get_player(world, mover));
  for (int idx = 0; idx < n_drawn; idx++) {
    rack_add_letter(own, bag2[idx]);
  }
  bag_set_to_tiles(game_get_bag(world), bag2 + n_drawn, 2 - n_drawn);
  if (!rack_is_empty(own)) {
    game_set_game_end_reason(world, GAME_END_REASON_NONE);
  }
}

int peg_world_all_placements(const Game *game, MoveList *wide, Move *moves) {
  move_list_reset(wide);
  const MoveGenArgs args = {.game = game,
                            .move_list = wide,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  move_list_sort_moves(wide);
  int count = 0;
  for (int rank = 0; rank < move_list_get_count(wide); rank++) {
    const Move *move = move_list_get_move(wide, rank);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      // Exchanges are never candidates (legal only with a full rack's worth
      // of tiles in the bag).
      if (bag_get_letters(game_get_bag(game)) < RACK_SIZE) {
        log_fatal("peg2 unexpected exchange");
      }
      continue;
    }
    move_copy(&moves[count++], move);
  }
  return count;
}

void peg_world_set_bag_pair(const int *unseen, const Rack *own_after,
                            const MachineLetter *bag2, int n_drawn, int mover,
                            int on_turn, Game *base) {
  int rest[VALUE_NET_TILE_TYPES];
  memcpy(rest, unseen, sizeof(rest));
  rest[bag2[0]]--;
  rest[bag2[1]]--;
  Rack *opponent = player_get_rack(game_get_player(base, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    for (int copy = 0; copy < rest[ml]; copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  Rack *own = player_get_rack(game_get_player(base, mover));
  rack_copy(own, own_after);
  for (int idx = 0; idx < n_drawn; idx++) {
    rack_add_letter(own, bag2[idx]);
  }
  bag_set_to_tiles(game_get_bag(base), bag2 + n_drawn, 2 - n_drawn);
  game_set_game_end_reason(base, GAME_END_REASON_NONE);
  game_set_player_on_turn_index(base, on_turn);
}

bool peg_world_cache_enabled(void) {
  static int cache = -1;
  if (cache < 0) {
    const char *value = getenv("PEG_WORLD_NO_CACHE");
    cache = value == NULL || atoi(value) == 0;
  }
  return cache == 1;
}

bool peg_world_swap_hidden_tiles(const Game *root, Game *swapped) {
  const int mover = game_get_player_on_turn_index(root);
  int actual[2] = {-1, -1}, found = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    for (int copy = 0;
         copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml) &&
         found < 2;
         copy++) {
      actual[found++] = ml;
    }
  }
  int full_unseen[VALUE_NET_TILE_TYPES], rest[VALUE_NET_TILE_TYPES];
  peg_world_public_unseen(root, full_unseen);
  memcpy(rest, full_unseen, sizeof(rest));
  rest[actual[0]]--;
  rest[actual[1]]--;
  MachineLetter swap2[2];
  int picked = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES && picked < 2; ml++) {
    while (rest[ml] > 0 && picked < 2 && ml != actual[0] && ml != actual[1]) {
      swap2[picked++] = (MachineLetter)ml;
      rest[ml]--;
    }
  }
  if (picked < 2) {
    return false;
  }
  Move pass;
  move_set_as_pass(&pass);
  peg_world_after_candidate(root, full_unseen, &pass, swap2, 0, swapped);
  bag_set_to_tiles(game_get_bag(swapped), swap2, 2);
  game_set_player_on_turn_index(swapped, mover);
  return true;
}
