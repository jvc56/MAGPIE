#include "root_candidates.h"

#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/root_leaves.h"
#include "gameplay.h"
#include "move_gen.h"
#include <string.h>

enum {
  // How many of the player's own best static moves are re-ranked. The KLV3
  // term moves a leave by a few points, so a move outside this pool cannot
  // reach a root candidate list of ordinary size.
  ROOT_CANDIDATES_POOL_CAPACITY = 1000,
};

void root_candidates_compute_adjustments(
    const RootLeaves *root_leaves, const Game *game,
    Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS]
                      [MACHINE_LETTER_MAX_VALUE]) {
  // The public unseen multiset: the bag plus the opponent's rack, which the
  // player on turn cannot tell apart.
  int unseen_counts[MACHINE_LETTER_MAX_VALUE];
  memset(unseen_counts, 0, sizeof(unseen_counts));
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  bag_increment_unseen_count(game_get_bag(game), unseen_counts);
  rack_increment_unseen_count(
      player_get_rack(game_get_player(game, 1 - player_on_turn_index)),
      unseen_counts);
  int unseen_total = 0;
  for (int ml = 0; ml < MACHINE_LETTER_MAX_VALUE; ml++) {
    unseen_total += unseen_counts[ml];
  }
  root_leaves_compute_tile_adjustments(root_leaves, unseen_counts, unseen_total,
                                       adjustments);
}

Equity root_candidates_get_move_adjustment(
    const Game *game,
    const Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS]
                            [MACHINE_LETTER_MAX_VALUE],
    const Move *move) {
  const int bag_tiles = bag_get_letters(game_get_bag(game));
  int draw_count = 0;
  switch (move_get_type(move)) {
  case GAME_EVENT_TILE_PLACEMENT_MOVE: {
    const int tiles_played = move_get_tiles_played(move);
    draw_count = tiles_played < bag_tiles ? tiles_played : bag_tiles;
    break;
  }
  case GAME_EVENT_EXCHANGE:
    draw_count = move_get_tiles_played(move);
    break;
  default:
    break;
  }
  if (draw_count <= 0) {
    return 0;
  }
  Rack leave;
  get_leave_for_move(move, game, &leave);
  return root_leaves_get_leave_adjustment(adjustments, &leave, draw_count);
}

static void generate_candidates(Game *game, MoveList *move_list) {
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
}

void generate_root_candidates(const RootLeaves *root_leaves, Game *game,
                              MoveList *move_list) {
  if (root_leaves == NULL) {
    generate_candidates(game, move_list);
    return;
  }
  MoveList *pool = move_list_create(ROOT_CANDIDATES_POOL_CAPACITY);
  generate_candidates(game, pool);
  Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE];
  root_candidates_compute_adjustments(root_leaves, game, adjustments);
  move_list_reset(move_list);
  move_list_set_rack(move_list, move_list_get_rack(pool));
  const int pool_count = move_list_get_count(pool);
  for (int move_idx = 0; move_idx < pool_count; move_idx++) {
    Move *move = move_list_get_move(pool, move_idx);
    move_set_equity(
        move, move_get_equity(move) +
                  root_candidates_get_move_adjustment(game, adjustments, move));
    move_list_add_move(move_list, move);
  }
  move_list_sort_moves(move_list);
  move_list_destroy(pool);
}
