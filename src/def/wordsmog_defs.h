#ifndef WORDSMOG_DEFS_H
#define WORDSMOG_DEFS_H

#include "rack_defs.h"

enum {
  // A square of a partial wordsmog arrangement is free, pinned to a natural
  // letter (its machine letter), or pinned to a blank whose designation is
  // still free.
  WORDSMOG_PIN_FREE = 0xFF,
  WORDSMOG_PIN_BLANK = 0x80,
  // The multisets of a rack's natural tiles: at most 2^RACK_SIZE, when every
  // tile differs.
  WORDSMOG_MAX_STATES = 1 << RACK_SIZE,
};

#endif
