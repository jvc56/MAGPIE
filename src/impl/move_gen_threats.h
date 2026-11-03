#ifndef MOVE_GEN_THREATS_H
#define MOVE_GEN_THREATS_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"

// The tiles the player not on turn might hold, as the player on turn sees
// them: the opponent's rack and the bag. With the bag empty and the
// opponent's rack known, that is exactly their rack.
void threats_get_unseen_tiles(const Game *game, Rack *unseen);

// The highest-scoring plays the player not on turn could make on the current
// board from `unseen` (see threats_get_unseen_tiles), playing at most
// RACK_SIZE tiles of it, as many as `threats` has capacity for, sorted by
// score from highest. A play's equity is its score.
//
// generate_moves needs the rack it plays from to be a rack, of at most
// RACK_SIZE tiles; the unseen tiles can be the whole bag. This searches the
// opponent's lexicon on the opponent's cross-sets like generate_moves, but
// records a play once whichever of the unseen tiles make it: a letter played
// more times than the unseen tiles hold it is played by blanks, which go
// where the letter scores least, so each play has the highest score the
// unseen tiles allow it. The search follows the classic rules, not those of
// another game variant.
void generate_threats(const Game *game, const Rack *unseen, MoveList *threats);

#endif
