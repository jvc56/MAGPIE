#ifndef EQUITY_STRING_H
#define EQUITY_STRING_H

#include "../ent/equity.h"
#include "../util/string_util.h"

void string_builder_add_equity(StringBuilder *sb, Equity equity,
                               const char *format);
#endif