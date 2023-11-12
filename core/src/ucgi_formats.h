#ifndef UCGI_FORMATS_H
#define UCGI_FORMATS_H

#include "string_util.h"

void string_builder_add_ucgi_move(const Move *move, const Board *board,
                                  const LetterDistribution *ld,
                                  StringBuilder *move_string_builder);

#endif
