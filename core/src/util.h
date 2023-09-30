#ifndef UTIL_H
#define UTIL_H

#include "klv.h"
#include "move.h"
#include "rack.h"

double get_leave_value_for_move(KLV *klv, Move *move, Rack *rack);
int prefix(const char *pre, const char *str);
void write_user_visible_letter(char *dest,
                               LetterDistribution *letter_distribution,
                               uint8_t ml);
int is_all_whitespace_or_empty(const char *str);

#endif
