#ifndef MOVE_STRING_H
#define MOVE_STRING_H

#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"

#include "../util/string_util.h"

void string_builder_add_move_description(const Move *move,
                                         const LetterDistribution *ld,
                                         StringBuilder *move_string_builder);
void string_builder_add_move(const Board *board, const Move *m,
                             const LetterDistribution *ld,
                             StringBuilder *string_builder);

void string_builder_add_ucgi_move(const Move *move, const Board *board,
                                  const LetterDistribution *ld,
                                  StringBuilder *move_string_builder);
#endif
