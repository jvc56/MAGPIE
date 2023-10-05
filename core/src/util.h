#ifndef UTIL_H
#define UTIL_H

#include "klv.h"
#include "move.h"
#include "rack.h"

double get_leave_value_for_move(KLV *klv, Move *move, Rack *rack);
int prefix(const char *pre, const char *str);
void *malloc_or_die(size_t size);
void *realloc_or_die(void *realloc_target, size_t size);
int char_to_int(char c);
int string_to_int(const char *str);

#endif
