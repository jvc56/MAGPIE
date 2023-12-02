#ifndef MOVE_STRING_H
#define MOVE_STRING_H

#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"

#include "string_util.h"

void string_builder_add_move_description(const Move *move,
                                         const LetterDistribution *ld,
                                         StringBuilder *move_string_builder);
void string_builder_add_move(const Board *board, const Move *m,
                             const LetterDistribution *letter_distribution,
                             StringBuilder *string_builder);
#endif
