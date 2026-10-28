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
  // With the PAT term on, how many plays a move generation prices with it by
  // default (see player_get_wordsmog_pat_evals). The exact search prices
  // hundreds for each placement that could hold the best play.
  WORDSMOG_DEFAULT_PAT_EVALS = 64,
  // Under MOVE_RECORD_ALL with the PAT term, the longest move list whose
  // entry threshold the search tracks to skip groups that cannot make it.
  WORDSMOG_PAT_MAX_TRACKED_LIST = 256,
};

#endif
