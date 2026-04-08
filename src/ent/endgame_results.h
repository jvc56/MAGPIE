#ifndef ENDGAME_RESULTS_H
#define ENDGAME_RESULTS_H

#include "../def/thread_control_defs.h"
#include "../ent/transposition_table.h"
#include "game.h"
#include "game_history.h"
#include "move.h"
#include <stdatomic.h>

// We don't expect an endgame length to ever be larger than this value.
enum { MAX_VARIANT_LENGTH = 25 };

typedef enum {
  ENDGAME_RESULT_BEST,
  ENDGAME_RESULT_DISPLAY,
} endgame_result_t;

typedef struct PVLine {
  SmallMove moves[MAX_VARIANT_LENGTH];
  Game *game;
  int32_t score;
  int num_moves;
  int negamax_depth; // number of moves from exact search (rest are greedy)
} PVLine;

typedef struct EndgameResults EndgameResults;

EndgameResults *endgame_results_create(void);
void endgame_results_destroy(EndgameResults *endgame_results);
void endgame_results_reset(EndgameResults *endgame_results);
bool endgame_results_get_valid_for_current_game_state(
    const EndgameResults *endgame_results);
void endgame_results_set_valid_for_current_game_state(
    EndgameResults *endgame_results, bool valid);
const PVLine *endgame_results_get_pvline(const EndgameResults *endgame_results,
                                         endgame_result_t result_type);
int endgame_results_get_value(const EndgameResults *endgame_results,
                              endgame_result_t result_type);
int endgame_results_get_depth(const EndgameResults *endgame_results,
                              endgame_result_t result_type);
double
endgame_results_get_seconds_elapsed(const EndgameResults *endgame_results);
void endgame_results_lock(EndgameResults *endgame_results,
                          endgame_result_t type);
void endgame_results_unlock(EndgameResults *endgame_results,
                            endgame_result_t type);
void endgame_results_update_display_data(EndgameResults *endgame_results);
void endgame_results_set_best_pvline(EndgameResults *endgame_results,
                                     const PVLine *pv_line, int value,
                                     int depth);
void endgame_results_set_start_game(EndgameResults *endgame_results,
                                    const Game *game);
const Game *
endgame_results_get_start_game(const EndgameResults *endgame_results);
void endgame_results_stop_ctimer(EndgameResults *endgame_results);
// Ensure the multi-PV array has room for at least n entries (reallocs if
// needed). Callers write directly via endgame_results_get_pvs_writable.
void endgame_results_ensure_pvs_capacity(EndgameResults *endgame_results,
                                         int n);
// Return a writable pointer to the multi-PV array.
PVLine *endgame_results_get_pvs_writable(EndgameResults *endgame_results);
void endgame_results_set_num_pvs(EndgameResults *endgame_results, int num_pvs);
int endgame_results_get_num_pvs(const EndgameResults *endgame_results);
const PVLine *
endgame_results_get_multi_pvline(const EndgameResults *endgame_results,
                                 int idx);
void endgame_results_set_pvline_extend_args(EndgameResults *endgame_results,
                                            TranspositionTable *tt,
                                            int solving_player, int max_depth);
TranspositionTable *
endgame_results_get_tt(const EndgameResults *endgame_results);
int endgame_results_get_solving_player(const EndgameResults *endgame_results);
int endgame_results_get_max_depth(const EndgameResults *endgame_results);
// Ensure the extended-PV array has room for at least n entries.
void endgame_results_ensure_extended_pvs_capacity(
    EndgameResults *endgame_results, int n);
// Return a writable pointer to the extended-PV array.
PVLine *
endgame_results_get_extended_pvs_writable(EndgameResults *endgame_results);
// Prepare the shared ext_game for PV extension: creates it via game_duplicate
// on first call, then resets it via game_copy on subsequent calls. Always sets
// endgame solving mode. Returns the ready-to-use game.
Game *endgame_results_prepare_ext_game(EndgameResults *endgame_results,
                                       const Game *source_game);

#endif
