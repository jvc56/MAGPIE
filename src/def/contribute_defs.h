#ifndef CONTRIBUTE_DEFS_H
#define CONTRIBUTE_DEFS_H

// JSON key names for the payloads MAGPIE submits to the birdtest server as a
// contribute worker. These keys are part of the wire contract between the
// server and this client, so every writer uses these constants instead of a
// raw string literal -- this is the one place to look, or change, the name
// of a key.

// Game recorder ("games" job type): written once per GameData set
// (all_games, and divergent_games for a paired run).
#define CONTRIBUTE_KEY_ALL_GAMES "all_games"
#define CONTRIBUTE_KEY_DIVERGENT_GAMES "divergent_games"
#define CONTRIBUTE_KEY_GAMES "games"
#define CONTRIBUTE_KEY_WINS "wins"
#define CONTRIBUTE_KEY_LOSSES "losses"
#define CONTRIBUTE_KEY_TIES "ties"
#define CONTRIBUTE_KEY_P1_SCORE_MEAN "p1_score_mean"
#define CONTRIBUTE_KEY_P1_SCORE_SD "p1_score_sd"
#define CONTRIBUTE_KEY_P2_SCORE_MEAN "p2_score_mean"
#define CONTRIBUTE_KEY_P2_SCORE_SD "p2_score_sd"

// Positions recorder ("positions" autoplay option) and opening-rack
// analysis, which share the same per-move shape.
#define CONTRIBUTE_KEY_POSITIONS "positions"
#define CONTRIBUTE_KEY_GAME_INDEX "game_index"
#define CONTRIBUTE_KEY_TURN_NUMBER "turn_number"
#define CONTRIBUTE_KEY_RACK "rack"
#define CONTRIBUTE_KEY_RACKS "racks"
#define CONTRIBUTE_KEY_NUM_MOVES "num_moves"
#define CONTRIBUTE_KEY_TOTAL_ITERATIONS "total_iterations"
#define CONTRIBUTE_KEY_TIME_ELAPSED "time_elapsed"
#define CONTRIBUTE_KEY_STATUS "status"
#define CONTRIBUTE_KEY_MOVES "moves"
#define CONTRIBUTE_KEY_MOVE "move"
#define CONTRIBUTE_KEY_SCORE "score"
#define CONTRIBUTE_KEY_EQUITY "equity"
#define CONTRIBUTE_KEY_WIN_PERCENTAGE "win_percentage"
#define CONTRIBUTE_KEY_ITERATIONS "iterations"
#define CONTRIBUTE_KEY_PLIES "plies"
#define CONTRIBUTE_KEY_PLY "ply"
#define CONTRIBUTE_KEY_BINGO_PERCENTAGE "bingo_percentage"
#define CONTRIBUTE_KEY_AVERAGE_SCORE "average_score"

#endif
