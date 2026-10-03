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

// The spread in points of the player on turn in game just after making
// move, from that player's side (the input row's spread scalar).
double value_net_spread_after_move(const Game *game, const Move *move);

// MAGPIE's score+win utility (sim_args.h's sim_utility_blend) of the
// position after a candidate, from the net's value and spread for its row:
// win% is (1 + value) / 2, draws counting half, and the final spread is
// spread_after (value_net_spread_after_move) plus the spread head's
// predicted change to the end of the game, 130 * atanh(spread). With
// w_spread 0 it is the win% alone and spread is not read.
double value_net_utility(float value, float spread, double spread_after,
                         double w_winpct, double w_spread, double spread_scale);

// The predicted final spread in points of the row's mover: spread_after
// plus 130 * atanh(spread), as in value_net_utility.
double value_net_final_spread(float spread, double spread_after);

#endif
