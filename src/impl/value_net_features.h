#ifndef VALUE_NET_FEATURES_H
#define VALUE_NET_FEATURES_H

#include "../ent/game.h"
#include "../ent/move.h"
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

void value_net_history_reset(ValueNetHistory *history);

// Updates the history of the player who did not make move: call for every
// move played, with history being that of the mover's opponent.
void value_net_history_record_opponent_move(ValueNetHistory *history,
                                            const Move *move);

// Writes the value net's input row (value_net_defs.h; board_row
// VALUE_NET_BOARD_FLOATS floats, scalars_row VALUE_NET_SCALARS) for the
// player on turn in game making move: the position after the move, from
// the mover's side, holding only the leave, nothing drawn, with the
// opponent's rack counted as unseen, as Macondo's BuildMLVector builds it.
// scratch is overwritten (a copy of game is played on it); game is not
// changed. Requires an English 15x15 game with tiles in the bag.
void value_net_features_for_move(const Game *game, const Move *move,
                                 const ValueNetHistory *history, Game *scratch,
                                 float *board_row, float *scalars_row);

#endif
