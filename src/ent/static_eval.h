#ifndef STATIC_EVAL_H
#define STATIC_EVAL_H

#include "../ent/board.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"

double static_eval_get_shadow_equity(const LetterDistribution *ld,
                                     const Rack *opp_rack,
                                     const double *best_leaves,
                                     const int *descending_tile_scores,
                                     int number_of_tiles_in_bag,
                                     int number_of_letters_on_rack,
                                     int tiles_played);

double static_eval_get_move_equity_with_leave_value(
    const LetterDistribution *ld, const Move *move, const Board *board,
    const Rack *player_leave, const Rack *opp_rack, int number_of_tiles_in_bag,
    double leave_value);

double static_eval_get_move_equity(const LetterDistribution *ld, const KLV *klv,
                                   const Move *move, const Board *board,
                                   const Rack *player_leave,
                                   const Rack *opp_rack,
                                   int number_of_tiles_in_bag);

int static_eval_get_move_score(const LetterDistribution *ld, const Move *move,
                               const Board *board, int cross_set_index);
#endif
