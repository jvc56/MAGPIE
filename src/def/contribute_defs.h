#ifndef CONTRIBUTE_DEFS_H
#define CONTRIBUTE_DEFS_H

#include "thread_control_defs.h"

// JSON key names for what MAGPIE exchanges with the birdtest server as a
// contribute worker -- both the task requests it reads and the result
// payloads it submits. These keys are part of the wire contract between the
// server and this client, so every reader and writer uses these constants
// instead of a raw string literal -- this is the one place to look, or
// change, the name of a key.

// The HTTP client's retry budget for a transport failure or a 5xx: this many
// retries, waiting 1, 2, 4, ... seconds and never more than the ceiling between
// attempts -- about fifteen minutes in all. Sized to outlast a birdtest
// deployment, which stops the one server instance before it starts the next;
// see http_client_backoff_seconds. Here rather than in http_client.c so the
// test that holds the budget to that purpose can read it.
enum {
  HTTP_CLIENT_MAX_TRANSIENT_RETRIES = 20,
  HTTP_CLIENT_MAX_BACKOFF_SECONDS = 60,
  HTTP_CLIENT_MAX_RATE_LIMIT_WAIT_SECONDS = 60,
};

// The longest a worker waits between claims while the server's leave KLV is
// missing or does not match its recorded hash (the wait doubles from the idle
// interval up to this); see contribute_decline_derived_mismatch.
enum { CONTRIBUTE_BAD_ARTIFACT_MAX_WAIT_SECONDS = 600 };

// The most threads a task runs on. Move generation keeps a pool of MAX_THREADS
// per-thread generators, each held until its thread exits. A task at N threads
// runs N autoplay workers, each with one simulation or inference thread at a
// time (contribute plays one game per thread), plus the contribute thread,
// which keeps its generator across tasks, and possibly the thread that called
// contribute: 2N+2 at most. Past the pool, magpie exits on the task.
enum { CONTRIBUTE_MAX_THREADS = (MAX_THREADS - 1) / 2 };

// Task request fields, read in config.c's config_contribute_* functions.
#define CONTRIBUTE_KEY_LEXICON "lexicon"
#define CONTRIBUTE_KEY_VARIANT "variant"
#define CONTRIBUTE_KEY_SEED "seed"
#define CONTRIBUTE_KEY_NUM_GAMES "num_games"
#define CONTRIBUTE_KEY_PLAYER "player"
#define CONTRIBUTE_KEY_PLAYER1 "player1"
#define CONTRIBUTE_KEY_PLAYER2 "player2"
#define CONTRIBUTE_KEY_LEAVES "leaves"
#define CONTRIBUTE_KEY_RECORDER_TYPE "recorder_type"
#define CONTRIBUTE_KEY_SORT_STRATEGY "sort_strategy"
#define CONTRIBUTE_KEY_MAX_ITERATIONS "max_iterations"
// How many plies to simulate (-pl1/-pl2) and how many plays to generate and
// simulate (-np1/-np2). These were once read as "plies" and "top_plays",
// which birdtest never sent, so every simming player silently ran on the
// worker's own ambient plies and play count.
#define CONTRIBUTE_KEY_NUM_PLIES "num_plies"
#define CONTRIBUTE_KEY_NUM_PLAYS "num_plays"
// How many plies to report (shplies). Read from player1, like
// num_plays_recorded: MAGPIE has one value for the whole run.
#define CONTRIBUTE_KEY_NUM_PLIES_RECORDED "num_plies_recorded"
// The job's pinned letter distribution (-ld) and board layout (-bdn), whose
// digests the worker verifies before running the task.
#define CONTRIBUTE_KEY_LETTER_DISTRIBUTION "letter_distribution"
#define CONTRIBUTE_KEY_BOARD_LAYOUT "board_layout"
#define CONTRIBUTE_KEY_STOPPING_PCT "stopping_pct"
#define CONTRIBUTE_KEY_USE_INFERENCE "use_inference"
#define CONTRIBUTE_KEY_TIME_LIMIT_SECS "time_limit_secs"
#define CONTRIBUTE_KEY_CAPTURE_POSITIONS "capture_positions"
#define CONTRIBUTE_KEY_NUM_PLAYS_RECORDED "num_plays_recorded"
// Run-wide settings a request states once for the whole task rather than per
// player: the bingo bonus (-bb) every job type scores with, and the
// simulation cutoff (-cutoff) of the job types that can simulate. Stated
// rather than left to this build's compile-time defaults, so a task means the
// same thing on every MAGPIE release.
#define CONTRIBUTE_KEY_BINGO_BONUS "bingo_bonus"
#define CONTRIBUTE_KEY_SIM_CUTOFF "sim_cutoff"

// Remaining per-player options (-l1/-l2, -w1/-w2, -rit1/-rit2, -mi1/-mi2,
// -pc1/-pc2, -th1/-th2, -sa1/-sa2, -im1/-im2, -uwin1/-uwin2,
// -uspread1/-uspread2, -uspreadscale1/-uspreadscale2), sent inside "player"/
// "player1"/"player2" alongside the existing keys above.
#define CONTRIBUTE_KEY_PLAYER_LEXICON "lexicon"
#define CONTRIBUTE_KEY_USE_WORDMAP "use_wordmap"
#define CONTRIBUTE_KEY_USE_RIT "use_rit"
// The name to load this player's rack info table under. A table belongs to a
// (.kwg, .klv2) pair, so the server names it rather than letting the worker
// infer it from the lexicon -- see src/impl/config.c's
// config_contribute_ensure_rack_info_table.
#define CONTRIBUTE_KEY_RIT_NAME "rit_name"
#define CONTRIBUTE_KEY_MIN_PLAY_ITERATIONS "min_play_iterations"
#define CONTRIBUTE_KEY_THRESHOLD "threshold"
#define CONTRIBUTE_KEY_SAMPLING_RULE "sampling_rule"
#define CONTRIBUTE_KEY_INFERENCE_MARGIN "inference_margin"
#define CONTRIBUTE_KEY_UTILITY_W_WINPCT "utility_w_winpct"
#define CONTRIBUTE_KEY_UTILITY_W_SPREAD "utility_w_spread"
#define CONTRIBUTE_KEY_UTILITY_SPREAD_SCALE "utility_spread_scale"

// Options that are one shared MAGPIE setting for the whole run rather than
// per-player, but which birdtest still sends once per player (validated
// equal between player1/player2 server-side) so nothing about play is left
// to a worker's own ambient config. Read from player1's object; see
// config_contribute_apply_shared_settings.
#define CONTRIBUTE_KEY_WIN_PCT_MODEL "win_pct_model"
#define CONTRIBUTE_KEY_MOVEGEN_MARGIN "movegen_margin"

// Endgame and pre-endgame (PEG) solving, per player, for games and game-pairs
// tasks (see AutoplaySolverSettings). endgame_plies and peg_max_bag are stated
// by every player, if only as 0; endgame_plies 0 turns off both solvers. The
// PEG keys are stated exactly when peg_max_bag > 0, and the nested keys
// exactly when peg_nested is true.
#define CONTRIBUTE_KEY_ENDGAME_PLIES "endgame_plies"
#define CONTRIBUTE_KEY_PEG_MAX_BAG "peg_max_bag"
#define CONTRIBUTE_KEY_PEG_STAGE_TOP_K "peg_stage_top_k"
#define CONTRIBUTE_KEY_PEG_SCENARIO_STRIDE "peg_scenario_stride"
#define CONTRIBUTE_KEY_PEG_OPP_MODEL "peg_opp_model"
#define CONTRIBUTE_KEY_PEG_NESTED "peg_nested"
#define CONTRIBUTE_KEY_PEG_NESTED_CAND_CAPS "peg_nested_cand_caps"
#define CONTRIBUTE_KEY_PEG_NESTED_MAX_DEPTH "peg_nested_max_depth"
// One stride per inner bag size, bags 1..PEG_MAX_BAG in order.
#define CONTRIBUTE_KEY_PEG_NESTED_STRIDES "peg_nested_strides"
#define CONTRIBUTE_PEG_OPP_MODEL_RATIONAL "rational"
#define CONTRIBUTE_PEG_OPP_MODEL_PESSIMISTIC "pessimistic"

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
// Paired runs only: the pentanomial distribution over completed pairs. Five
// counts, indexed by player 1's score across the pair in half-points, so
// index 0 is "player 1 lost both games" and index 4 is "player 1 won both".
// Every completed pair lands in exactly one bucket, including the pairs whose
// two games played identically -- those are guaranteed 1-1 ties and land in
// index 2. A consumer that drops them is conditioning on the outcome and will
// badly overstate the difference between the players.
#define CONTRIBUTE_KEY_PENTANOMIAL "pentanomial"

// Positions recorder ("positions" autoplay option) and opening-rack
// analysis, which share the same per-move shape.
#define CONTRIBUTE_KEY_POSITIONS "positions"
#define CONTRIBUTE_KEY_GAME_INDEX "game_index"
#define CONTRIBUTE_KEY_TURN_NUMBER "turn_number"
#define CONTRIBUTE_KEY_RACK "rack"
#define CONTRIBUTE_KEY_RACKS "racks"
#define CONTRIBUTE_KEY_POSITION "position"
#define CONTRIBUTE_KEY_PREVIOUS_MOVE "previous_move"
#define CONTRIBUTE_KEY_PREVIOUS_MOVE_SCORE "previous_move_score"
#define CONTRIBUTE_KEY_NUM_MOVES "num_moves"
#define CONTRIBUTE_KEY_TOTAL_ITERATIONS "total_iterations"
#define CONTRIBUTE_KEY_TIME_ELAPSED "time_elapsed"
#define CONTRIBUTE_KEY_STATUS "status"
#define CONTRIBUTE_KEY_MOVES "moves"
#define CONTRIBUTE_KEY_MOVE "move"
#define CONTRIBUTE_KEY_SCORE "score"
#define CONTRIBUTE_KEY_EQUITY "equity"
#define CONTRIBUTE_KEY_WIN_PERCENTAGE "win_percentage"
#define CONTRIBUTE_KEY_BLENDED_UTILITY "blended_utility"
#define CONTRIBUTE_KEY_ITERATIONS "iterations"
#define CONTRIBUTE_KEY_PLIES "plies"
#define CONTRIBUTE_KEY_PLY "ply"
#define CONTRIBUTE_KEY_BINGO_PERCENTAGE "bingo_percentage"
#define CONTRIBUTE_KEY_AVERAGE_SCORE "average_score"
// How a captured in-game position was analysed: one of the
// CONTRIBUTE_ANALYSIS_* names. A PEG or endgame position's moves carry the
// solver's mean_spread (the mover's projected final spread, in points) and
// fidelity_plies (the depth the move was ranked at).
#define CONTRIBUTE_KEY_ANALYSIS "analysis"
#define CONTRIBUTE_KEY_MEAN_SPREAD "mean_spread"
#define CONTRIBUTE_KEY_FIDELITY_PLIES "fidelity_plies"
#define CONTRIBUTE_ANALYSIS_STATIC "static"
#define CONTRIBUTE_ANALYSIS_SIM "sim"
#define CONTRIBUTE_ANALYSIS_PEG "peg"
#define CONTRIBUTE_ANALYSIS_ENDGAME "endgame"

// Leave generation ("leave_generation" job type).
#define CONTRIBUTE_KEY_GENERATION "generation"
#define CONTRIBUTE_KEY_FORCED_RACKS "forced_racks"
#define CONTRIBUTE_KEY_PREVIOUS_ARTIFACT_KEY "previous_artifact_key"
#define CONTRIBUTE_KEY_PREVIOUS_ARTIFACT_SHA256 "previous_artifact_sha256"
// There is deliberately no target_rack_count: the generation's rack target is
// server-only state (see config_contribute_leave_gen), and a task ends on
// num_games alone.
#define CONTRIBUTE_KEY_COUNT "count"
#define CONTRIBUTE_KEY_MEAN "mean"

#endif
