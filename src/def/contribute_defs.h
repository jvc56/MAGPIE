#ifndef CONTRIBUTE_DEFS_H
#define CONTRIBUTE_DEFS_H

// JSON key names for what MAGPIE exchanges with the birdtest server as a
// contribute worker -- both the task requests it reads and the result
// payloads it submits. These keys are part of the wire contract between the
// server and this client, so every reader and writer uses these constants
// instead of a raw string literal -- this is the one place to look, or
// change, the name of a key.

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
#define CONTRIBUTE_KEY_TOP_PLAYS "top_plays"
#define CONTRIBUTE_KEY_STOPPING_PCT "stopping_pct"
#define CONTRIBUTE_KEY_USE_INFERENCE "use_inference"
#define CONTRIBUTE_KEY_TIME_LIMIT_SECS "time_limit_secs"
#define CONTRIBUTE_KEY_CAPTURE_POSITIONS "capture_positions"
#define CONTRIBUTE_KEY_NUM_PLAYS_RECORDED "num_plays_recorded"

// Remaining per-player options (-l1/-l2, -w1/-w2, -rit1/-rit2, -mi1/-mi2,
// -pc1/-pc2, -th1/-th2, -sa1/-sa2, -im1/-im2, -uwin1/-uwin2,
// -uspread1/-uspread2, -uspreadscale1/-uspreadscale2), sent inside "player"/
// "player1"/"player2" alongside the existing keys above.
#define CONTRIBUTE_KEY_PLAYER_LEXICON "lexicon"
#define CONTRIBUTE_KEY_USE_WORDMAP "use_wordmap"
#define CONTRIBUTE_KEY_USE_RIT "use_rit"
#define CONTRIBUTE_KEY_MIN_PLAY_ITERATIONS "min_play_iterations"
#define CONTRIBUTE_KEY_PLAY_CHOOSER_TIME_SECS "play_chooser_time_secs"
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
#define CONTRIBUTE_KEY_ENDGAME_PLIES "endgame_plies"
#define CONTRIBUTE_KEY_ENDGAME_TOP_K "endgame_top_k"
#define CONTRIBUTE_KEY_ENDGAME_TIME_LIMIT_SECS "endgame_time_limit_secs"
#define CONTRIBUTE_KEY_PRE_ENDGAME_TOP_K "pre_endgame_top_k"
#define CONTRIBUTE_KEY_PRE_ENDGAME_TIME_LIMIT_SECS "pre_endgame_time_limit_secs"
#define CONTRIBUTE_KEY_PRE_ENDGAME_STRIDE "pre_endgame_stride"
#define CONTRIBUTE_KEY_PRE_ENDGAME_NO_PRUNE "pre_endgame_no_prune"
#define CONTRIBUTE_KEY_PRE_ENDGAME_PESSIMISTIC "pre_endgame_pessimistic"
#define CONTRIBUTE_KEY_PRE_ENDGAME_NESTED "pre_endgame_nested"

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

// Leave generation ("leave_generation" job type).
#define CONTRIBUTE_KEY_GENERATION "generation"
#define CONTRIBUTE_KEY_FORCED_RACKS "forced_racks"
#define CONTRIBUTE_KEY_PREVIOUS_ARTIFACT_KEY "previous_artifact_key"
// The minimum number of times every forced rack must occur before this
// generation closes -- leavegen's per-generation "minimum rack target", the
// value the CLI passes positionally as one entry of e.g.
// "100,200,500,1000,1000,1000".
#define CONTRIBUTE_KEY_TARGET_RACK_COUNT "target_rack_count"
#define CONTRIBUTE_KEY_COUNT "count"
#define CONTRIBUTE_KEY_MEAN "mean"

#endif
