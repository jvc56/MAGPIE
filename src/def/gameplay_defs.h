#ifndef GAMEPLAY_DEFS_H
#define GAMEPLAY_DEFS_H

// Negative return values of draw_rack_string_from_bag. Any non-negative
// return value is the number of letters drawn.
enum {
  DRAW_RACK_STRING_MALFORMED = -1,
  DRAW_RACK_STRING_NOT_IN_BAG = -2,
  DRAW_RACK_STRING_TOO_MANY_LETTERS = -3,
};

#endif
