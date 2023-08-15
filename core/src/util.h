#ifndef UTIL_H
#define UTIL_H

#include "klv.h"
#include "move.h"
#include "rack.h"

double get_leave_value_for_move(KLV *klv, Move *move, Rack *rack);
int prefix(const char *pre, const char *str);
void write_user_visible_letter_to_end_of_buffer(
    char *dest, LetterDistribution *letter_distribution, uint8_t ml);
void write_rack_to_end_of_buffer(char *dest,
                                 LetterDistribution *letter_distribution,
                                 Rack *rack);
#endif
