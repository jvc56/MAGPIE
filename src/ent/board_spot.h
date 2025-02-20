#ifndef BOARD_SPOT_H
#define BOARD_SPOT_H

#include <stdbool.h>
#include <stdint.h>

#include "../ent/bit_rack.h"

// A BoardSpot has information to aid in finding the highest-equity play
// possible given a starting square, direction, and number of tiles played.
// This is used in WMP-based move generation. BoardSpot is independent of rack
// tiles: move generation, which does know the rack, will take the BoardSpot
// and compute the inner product of the descending effective multipliers with
// the rack's descending tile scores to compute the rack-dependent part of the
// highest possible score for the play.
//
// Fixed based on indices elsewhere and not set here:
// * start row
// * start col
// * direction
// * cross set index
// * number of tiles
// Known but not explicit here:
// * word multiplier
//
// If is_usable is false, the other fields are not updated and have no meaning.
// However a BoardSpot can become usable, then unusable (based on hooks), then
// usable again (based on hooks changing), so updates to BoardSpots must not
// assume correctness of previously unusable spots.
typedef struct BoardSpot {
    BitRack playthrough_bit_rack;
    uint8_t descending_effective_multipliers[WORD_ALIGNING_RACK_SIZE];
    // additional_score is the sum of
    // 1. Playthrough tiles (already multiplied by overall word multiplier)
    // 2. Perpendicular score (hooked tiles, each already multiplied by their
    //    respective word multipliers at crossing squares)
    // 3. Bingo bonus if applicable
    Equity additional_score;
    // This spot is usable if
    // 1. The starting square is empty
    // 2. Enough squares are empty to fit the spot's tiles in this row
    // 3. All empty squares used are compatible with perpendicular tiles
    //     (all cross sets are nonempty)
    // 4. Tiles placed in this spot will connect to tiles already on the board
    //    or to the starting square
    bool is_usable;
    uint8_t word_length;
} BoardSpot;

#endif