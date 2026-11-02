#ifndef PEG_ADJUST_H
#define PEG_ADJUST_H

#include "../def/rack_defs.h"
#include "../def/static_eval_defs.h"
#include "../util/io_util.h"
#include "equity.h"

// Pre-endgame adjustments: amounts added to the static equity of a tile
// placement made while the bag holds tiles, which set how much a player
// wants to leave tiles in the bag or empty it. The table is laid out as
// Quackle's was: its index is the tiles in the bag before the play, less the
// tiles played, plus RACK_SIZE (Quackle's leftInBagPlusSeven). An index of
// RACK_SIZE or more leaves (index - RACK_SIZE) tiles in the bag; a smaller
// index empties the bag and leaves the mover index tiles. Plays that leave
// PEG_ADJUST_VALUES_LENGTH - RACK_SIZE or more tiles in the bag get no
// adjustment, nor do exchanges and passes, which leave the bag as it is.
typedef struct PegAdjust PegAdjust;

// Reads <data path>/strategy/<name>.csv. Each row is "index,value", the
// value in points; an index left out is 0. Blank rows and rows starting
// with '#' are skipped.
PegAdjust *peg_adjust_create(const char *data_paths, const char *name,
                             ErrorStack *error_stack);
// A table of the given values (in millipoints), named name.
PegAdjust *
peg_adjust_create_from_values(const char *name,
                              const Equity values[PEG_ADJUST_VALUES_LENGTH]);
void peg_adjust_destroy(PegAdjust *peg_adjust);

const char *peg_adjust_get_name(const PegAdjust *peg_adjust);
Equity peg_adjust_get_value(const PegAdjust *peg_adjust, int index);

// The adjustment of a tile placement of tiles_played tiles made from a bag
// of number_of_tiles_in_bag tiles: 0 when peg_adjust is NULL or the bag is
// empty.
Equity peg_adjust_get_placement_value(const PegAdjust *peg_adjust,
                                      int number_of_tiles_in_bag,
                                      int tiles_played);

// Sets by_tiles_played[tiles_played] to the adjustment of a tile placement of
// that many tiles from a bag of number_of_tiles_in_bag tiles, for 1 to
// RACK_SIZE tiles; by_tiles_played[0] is 0.
void peg_adjust_fill_by_tiles_played(const PegAdjust *peg_adjust,
                                     int number_of_tiles_in_bag,
                                     Equity by_tiles_played[RACK_SIZE + 1]);

#endif
