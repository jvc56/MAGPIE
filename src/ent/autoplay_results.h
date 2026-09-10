#ifndef AUTOPLAY_RESULTS_H
#define AUTOPLAY_RESULTS_H

#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include "game.h"
#include "klv.h"
#include "move.h"
#include "sim_results.h"
#include <stdbool.h>

typedef enum {
  AUTOPLAY_RECORDER_TYPE_GAME,
  AUTOPLAY_RECORDER_TYPE_FJ,
  AUTOPLAY_RECORDER_TYPE_WIN_PCT,
  AUTOPLAY_RECORDER_TYPE_LEAVES,
  AUTOPLAY_RECORDER_TYPE_POSITION,
  NUMBER_OF_AUTOPLAY_RECORDERS,
} autoplay_recorder_t;

typedef struct AutoplayResults AutoplayResults;

typedef struct AutoplayGameTiming {
  bool active[2];
  double seconds_used[2];
  double overtime_seconds[2];
  int penalty_points[2];
} AutoplayGameTiming;

AutoplayResults *autoplay_results_create(void);
AutoplayResults *
autoplay_results_create_empty_copy(const AutoplayResults *orig);
void autoplay_results_set_options(AutoplayResults *autoplay_results,
                                  const char *options_str,
                                  ErrorStack *error_stack);
void autoplay_results_destroy(AutoplayResults *autoplay_results);
void autoplay_results_reset(AutoplayResults *autoplay_results);

void autoplay_results_add_move(AutoplayResults *autoplay_results,
                               const Game *game, const Move *move,
                               const Move *previous_move, const Rack *leave,
                               const MoveList *move_list,
                               const SimResults *sim_results, int game_number,
                               int pair_game_number, int turn_number,
                               int play_cap);
void autoplay_results_add_game(AutoplayResults *autoplay_results,
                               const Game *game, int turns, bool divergent,
                               uint64_t seed);
void autoplay_results_add_game_with_timing(AutoplayResults *autoplay_results,
                                           const Game *game, int turns,
                                           bool divergent, uint64_t seed,
                                           const AutoplayGameTiming *timing);

// Records one completed game pair as a single pentanomial observation.
// `game1` and `game2` are the two games of the pair, which share a seed and
// differ only in which player moved first; player index 0 is the same player
// in both. The pair lands in the bucket given by player 0's score across the
// two games in half-points (0 = lost both, 2 = split, 4 = won both).
//
// Called once per pair, in addition to the two per-game
// autoplay_results_add_game calls, and only in paired mode. The pair -- not
// the game -- is the independent unit of a paired run, which is why the
// counts this builds are what a statistical test should consume: the two
// games of a pair share a seed and are not independent observations.
void autoplay_results_add_game_pair(AutoplayResults *autoplay_results,
                                    const Game *game1, const Game *game2);
void autoplay_results_consolidate(AutoplayResults **autoplay_results_list,
                                  int list_size, AutoplayResults *primary);

// Returns a finished autoplay run's results as a JSON object, one key per
// active recorder -- the same "each recorder decides how it writes itself"
// design as autoplay_results_to_string, just producing JSON instead of
// human/status text. This is what the contribution client submits.
//
// The returned string is owned by autoplay_results, not the caller: it is
// cached and freed on the next call (or when autoplay_results is destroyed),
// so callers that need to keep it past that point must copy it themselves.
//
// The game recorder (when active) writes "all_games" (game counts and score
// moments, read straight out of the recorder rather than parsed back out of
// formatted output) and, when `show_divergent` is true, two more entries
// describing the paired run:
//
//   "pentanomial"     five counts over *every* completed pair, indexed by
//                     player 1's half-point score across the pair. This is
//                     the statistically meaningful summary of a paired run:
//                     the pair is the independent unit, and pairs that played
//                     identically are 1-1 ties that belong in the sample.
//   "divergent_games" the subset whose two games did not play identically.
//                     A diagnostic -- it says how often the two players
//                     actually differ -- and NOT a sample to run a test on,
//                     since selecting it conditions on the outcome.
//
// The positions recorder (when active) writes "positions": each worker
// thread accumulates its own captures, and consolidation renders all of them
// into this recorder's share of the JSON, so they are *not* in game or turn
// order -- each carries its own game and turn number.
const char *autoplay_results_get_json(AutoplayResults *autoplay_results,
                                      bool show_divergent);

// Leave generation only. Set by postgen_prebroadcast_func once a
// generation's RackList target is reached (or leavegen_max_games ends the
// run first); NULL otherwise. See the struct comment in autoplay_results.c.
void autoplay_results_set_leave_results_json(AutoplayResults *autoplay_results,
                                             char *leave_results_json);
const char *autoplay_results_get_leave_results_json(
    const AutoplayResults *autoplay_results);

char *autoplay_results_to_string(AutoplayResults *autoplay_results,
                                 bool human_readable, bool show_divergent);
char *autoplay_results_get_status(AutoplayResults *autoplay_results);
void string_builder_add_winning_player_confidence(StringBuilder *sb,
                                                  double p0_total,
                                                  double p1_total,
                                                  uint64_t total_games);
void autoplay_results_set_write_buffer_size(AutoplayResults *autoplay_results,
                                            size_t write_buffer_size);
size_t
autoplay_results_get_write_buffer_size(AutoplayResults *autoplay_results);
void autoplay_results_set_data_paths(AutoplayResults *autoplay_results,
                                     const char *data_paths);
void autoplay_results_set_ld(AutoplayResults *autoplay_results,
                             const LetterDistribution *ld);
void autoplay_results_set_klv(AutoplayResults *autoplay_results, KLV *klv);
uint64_t autoplay_results_build_option(autoplay_recorder_t recorder_type);
uint64_t autoplay_results_get_options(const AutoplayResults *autoplay_results);
void autoplay_results_set_players_data(AutoplayResults *autoplay_results,
                                       const PlayersData *players_data);
void autoplay_results_set_play_chooser_config(
    AutoplayResults *autoplay_results, const bool active[2],
    const double time_control_seconds[2], int overtime_penalty_points,
    double overtime_period_seconds);
void autoplay_results_set_status_data(AutoplayResults *autoplay_results,
                                      AutoplayResults **results_list,
                                      int results_list_size, bool finished,
                                      bool human_readable, bool show_divergent);
#endif
