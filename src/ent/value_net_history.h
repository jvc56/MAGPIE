#ifndef VALUE_NET_HISTORY_H
#define VALUE_NET_HISTORY_H

#include "../def/game_history_defs.h"
#include "../def/rack_defs.h"
#include "move.h"
#include <stdbool.h>

// What the value net needs from the game's history, from the side of the
// player about to move.
typedef struct ValueNetHistory {
  // The opponent's last move (any type), if they have moved.
  bool has_opponent_last_move;
  Move opponent_last_move;
  // The opponent's moves since their last bingo, or all of their moves if
  // they have not bingoed.
  int opponent_moves_since_bingo;
} ValueNetHistory;

static inline void value_net_history_reset(ValueNetHistory *history) {
  history->has_opponent_last_move = false;
  history->opponent_moves_since_bingo = 0;
}

// Updates the history of the player who did not make move: call for every
// move played, with history being that of the mover's opponent.
static inline void
value_net_history_record_opponent_move(ValueNetHistory *history,
                                       const Move *move) {
  history->has_opponent_last_move = true;
  move_copy(&history->opponent_last_move, move);
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
      move_get_tiles_played(move) == RACK_SIZE) {
    history->opponent_moves_since_bingo = 0;
  } else {
    history->opponent_moves_since_bingo++;
  }
}

#endif
