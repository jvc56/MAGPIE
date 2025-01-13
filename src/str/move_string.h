#ifndef MOVE_STRING_H
#define MOVE_STRING_H

#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"

#include "../util/string_util.h"

void string_builder_add_move_description(StringBuilder *move_string_builder,
                                         const Move *move,
                                         const LetterDistribution *ld);
void string_builder_add_move(StringBuilder *string_builder, const Board *board,
                             const Move *m, const LetterDistribution *ld);                             

void string_builder_add_ucgi_move(StringBuilder *move_string_builder,
                                  const Move *move, const Board *board,
                                  const LetterDistribution *ld);
#endif
