#ifndef VALUE_NET_FEATURES_H
#define VALUE_NET_FEATURES_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/value_net_history.h"
#include <stdbool.h>

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
