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
// index empties the bag and leaves the mover index tiles.
//
// A table is as long as its file makes it: it has a value for each index up
// to the highest its file gives, which is below PEG_ADJUST_MAX_VALUES_LENGTH.
// Plays that reach an index past the table's last get no adjustment, nor do
// exchanges and passes, which leave the bag as it is. The index counts from
// the rack size, so a table is for the RACK_SIZE it was made with.
typedef struct PegAdjust PegAdjust;

// Reads <data path>/strategy/<name>.csv. Each row is "index,value", the
// value in points, finite and at most PEG_ADJUST_MAX_ABS_VALUE either way.
// The table has one value more than the highest index given; an index left
// out below that is 0. Blank rows and rows starting with '#' are skipped.
PegAdjust *peg_adjust_create(const char *data_paths, const char *name,
                             ErrorStack *error_stack);
// A table of the first number_of_values values (in millipoints), named name.
// number_of_values is at most PEG_ADJUST_MAX_VALUES_LENGTH.
PegAdjust *peg_adjust_create_from_values(const char *name, const Equity *values,
                                         int number_of_values);
void peg_adjust_destroy(PegAdjust *peg_adjust);

const char *peg_adjust_get_name(const PegAdjust *peg_adjust);
// One more than the table's last index.
int peg_adjust_get_number_of_values(const PegAdjust *peg_adjust);
// The value at an index below PEG_ADJUST_MAX_VALUES_LENGTH: 0 past the
// table's last.
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

// A number two bags share only when peg_adjust, which may be NULL, gives
// every tile placement the same adjustment from both. A play takes at most
// RACK_SIZE tiles, so its index is at least the tiles in the bag, and no play
// from a bag holding as many tiles as the table has values gets an
// adjustment: those bags share a class, and each smaller bag has its own. A
// table shorter than the standard length, and no table, class the bags as a
// table of the standard length does.
int peg_adjust_get_bag_class(const PegAdjust *peg_adjust,
                             int number_of_tiles_in_bag);

#endif
