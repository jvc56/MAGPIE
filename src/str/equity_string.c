#include "../def/equity_defs.h"
#include "../ent/equity.h"

#include "../util/string_util.h"

void string_builder_add_equity(StringBuilder *sb, Equity equity,
                               const char *format) {
  switch (equity) {
  case EQUITY_PASS_VALUE:
    string_builder_add_string(sb, "EQUITY_PASS_VALUE");
    return;
  case EQUITY_INITIAL_VALUE:
    string_builder_add_string(sb, "EQUITY_INITIAL_VALUE");
    return;
  case EQUITY_UNDEFINED_VALUE:
    string_builder_add_string(sb, "EQUITY_UNDEFINED_VALUE");
    return;
  case EQUITY_MIN_VALUE:
    string_builder_add_string(sb, "EQUITY_MIN_VALUE");
    return;
  case EQUITY_MAX_VALUE:
    string_builder_add_string(sb, "EQUITY_MAX_VALUE");
    return;
  default:
    string_builder_add_formatted_string(sb, format, equity_to_double(equity));
  }
}