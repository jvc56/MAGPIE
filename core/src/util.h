#ifndef UTIL_H
#define UTIL_H

#include "klv.h"
#include "move.h"
#include "rack.h"

double get_leave_value_for_move(KLV *klv, Move *move, Rack *rack);
int prefix(const char *pre, const char *str);

#endif
