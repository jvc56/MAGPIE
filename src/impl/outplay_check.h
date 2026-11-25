#ifndef OUTPLAY_CHECK_H
#define OUTPLAY_CHECK_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/outplay_check_weights.h"

// The static player's out-play check. With 1 to OUTPLAY_CHECK_MAX_BAG tiles
// in the real bag (see game_get_real_bag_size) and a full rack, the static
// player's best plays by static equity, up to OUTPLAY_CHECK_CANDIDATES, are
// valued again where they empty the bag. For each way the bag could fall
// (every distinct draw of the unseen tiles up to OUTPLAY_CHECK_MAX_DRAWS of
// them, weighted by its probability, and otherwise that many sampled draws),
// the mover holds its leave and the draw, and the opponent the rest of the
// unseen tiles, and the weights value that position from what the opponent,
// on turn, and then the mover could play (see outplay_check_weights.h and
// outplay_check_find_features). A candidate's value is its score plus the
// mean of those values, less the weights' center for its bag and leave size.
// Candidates that do not empty the bag keep their static equity. The
// opponent's rack may be known or not: its tiles count as unseen either way.
//
// Returns the play the check chooses: the first of move_list, the static
// player's best play, unless another candidate is valued above the best,
// which then replaces it in move_list. Does nothing when the weights are for
// another letter distribution than the game's.
void outplay_check_choose(Game *game, const MoveList *move_list,
                          const OutplayCheckWeights *weights);

// What the player on turn in game, the opponent, and then the other player,
// the mover, could play, as the check finds it for each draw: whether the
// opponent can go out, and its best out score or else its best score; whether
// the mover can go out with its whole rack, its best out score and the lanes
// its out plays lie in, counted up to 3. Leaves the opponent on turn.
void outplay_check_find_features(Game *game, OutplayCheckFeatures *features);

#endif
