#ifndef AUTOPLAY_RESULTS_H
#define AUTOPLAY_RESULTS_H

#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include "game.h"
#include "klv.h"
#include "move.h"
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
// One ranked candidate at a captured position.
typedef struct AutoplayPositionMove {
  char *move;
  int score;
  double equity;
} AutoplayPositionMove;

// A position analysed during a game, kept by the `positions` recorder. A player
// analyses a position on every turn anyway; this is that analysis, retained
// rather than discarded.
typedef struct AutoplayPosition {
  char *cgp;
  char *rack;
  int game_number;
  int pair_game_number;
  int turn_number;
  // How many moves were ranked, before truncation to the recorder's cap.
  int num_moves;
  AutoplayPositionMove *moves;
  int num_stored_moves;
} AutoplayPosition;

// The positions captured by the last run, or NULL with *count 0 when the
// recorder is not enabled. Borrowed: owned by the recorder and valid until the
// next autoplay run.
//
// Positions are recorded per worker thread and merged, so they are *not* in
// game or turn order -- each carries its own game and turn number.
const AutoplayPosition *
autoplay_results_get_positions(const AutoplayResults *autoplay_results,
                               int *count);

// Ranked moves to keep per captured position. Set before the run.
void autoplay_results_set_position_move_cap(AutoplayResults *autoplay_results,
                                            int cap);

void autoplay_results_add_move(AutoplayResults *autoplay_results,
                               const Game *game, const Move *move,
                               const Rack *leave, const MoveList *move_list,
                               int game_number, int pair_game_number,
                               int turn_number);
void autoplay_results_add_game(AutoplayResults *autoplay_results,
                               const Game *game, int turns, bool divergent,
                               uint64_t seed);
void autoplay_results_add_game_with_timing(AutoplayResults *autoplay_results,
                                           const Game *game, int turns,
                                           bool divergent, uint64_t seed,
                                           const AutoplayGameTiming *timing);
void autoplay_results_consolidate(AutoplayResults **autoplay_results_list,
                                  int list_size, AutoplayResults *primary);
// Writes a finished autoplay run's game counts and score moments -- read
// straight out of the recorder rather than parsed back out of its formatted
// output -- as a JSON object under `key` into `sb`. This is what the
// contribution client submits. `first` guards comma placement, as with the
// other json_write_* helpers.
//
// When `divergent` is true the summary covers only game pairs whose two games
// did not play identically -- pairs that played identically are guaranteed
// ties carrying no information, so that subset is where a paired run's signal
// lives.
//
// Returns false (writing nothing) if the game recorder is not enabled.
bool autoplay_results_write_game_summary(
    const AutoplayResults *autoplay_results, StringBuilder *sb, const char *key,
    bool divergent, bool *first);

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
