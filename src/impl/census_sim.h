#ifndef CENSUS_SIM_H
#define CENSUS_SIM_H

#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"

// An exhaustive one-ply sim by the census: a move's value is what a one-ply
// sim's mean converges to, its score and leave less the expected static
// equity (score and leave) of the opponent's best reply, over every rack the
// opponent can hold, each weighted by the ways to draw it from the tiles the
// mover cannot see. One census a move: the board after the move, its play
// scores over the unseen pool, and each drawable rack's best split.
//
// The census lets the opponent exchange any tiles, so the values hold while
// the bag keeps at least RACK_SIZE tiles after the move. The opponent's word
// graph must be the first player's (as census_play_scores_build has it).
typedef struct CensusSim CensusSim;

// Times of the two stages of the census_sim_value_move calls so far.
typedef struct CensusSimTimes {
  double play_scores_seconds;
  double expected_best_seconds;
  int number_of_moves;
} CensusSimTimes;

// The play scores and the expected best reply run on number_of_threads
// threads (at most CENSUS_SIM_MAX_THREADS).
CensusSim *census_sim_create(const Game *game, int number_of_threads);
void census_sim_destroy(CensusSim *sim);

// The one-ply value of the player on turn's move in game: its score and leave
// less *expected_reply, the expected score and leave of the opponent's best
// reply (when expected_reply is not NULL). scratch is a game the move is
// played on, overwritten.
Equity census_sim_value_move(CensusSim *sim, const Game *game, const Move *move,
                             Game *scratch, Equity *expected_reply);

// The threads the play scores and the expected best reply run on.
void census_sim_set_threads(CensusSim *sim, int number_of_threads);

const CensusSimTimes *census_sim_get_times(const CensusSim *sim);

enum {
  // The groups of lanes whose play scores census_sim_set_position keeps.
  CENSUS_SIM_LANE_GROUPS = 6,
  // The most threads a census sim runs on.
  CENSUS_SIM_MAX_THREADS = 64,
};

// Sets the position whose moves census_sim_value_move values next: it builds
// the play scores of each group of lanes on the position's board once, and
// each move then finds again only those of the groups whose lanes it changes.
// The values are the same as without a position.
void census_sim_set_position(CensusSim *sim, const Game *game);
// Has census_sim_value_move build every move's play scores whole.
void census_sim_clear_position(CensusSim *sim);

#endif
