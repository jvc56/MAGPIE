#ifndef NERFED_PLAYER_H
#define NERFED_PLAYER_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"

// A NerfedPlayer models a human of a target rating. Each candidate play is
// independently visible with a probability fitted from the annotated-game
// corpus (word knowledge x geometry), and visible plays are valued as
// static equity plus Gumbel noise with a rating-dependent scale. The player
// chooses the best noisy-valued visible play.
typedef struct NerfedPlayer NerfedPlayer;

// Loads data/lexica/<lexicon>_wordfeats.csv for the game's player 0 lexicon.
NerfedPlayer *nerfed_player_create(const Game *game, int rating,
                                   ErrorStack *error_stack);
void nerfed_player_destroy(NerfedPlayer *nerfed_player);

// Generates all moves for the position and returns the nerfed selection.
// The returned move points into the nerfed player's internal move list and
// is valid until the next call.
const Move *nerfed_player_select_move(NerfedPlayer *nerfed_player, Game *game,
                                      XoshiroPRNG *prng);

#endif
