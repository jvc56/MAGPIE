#ifndef ENDGAME_RESULTS_H
#define ENDGAME_RESULTS_H

#include "../def/thread_control_defs.h"
#include "game.h"
#include "game_history.h"
#include "move.h"

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
double endgame_results_get_display_seconds_elapsed(
    const EndgameResults *endgame_results);
void endgame_results_lock(EndgameResults *endgame_results,
                          endgame_result_t type);
void endgame_results_unlock(EndgameResults *endgame_results,
                            endgame_result_t type);
void endgame_results_update_display_data(EndgameResults *endgame_results);
void endgame_results_set_best_pvline(EndgameResults *endgame_results,
                                     const PVLine *pv_line, int value,
                                     int depth, bool partial);
bool endgame_results_get_partial(const EndgameResults *endgame_results,
                                 endgame_result_t result_type);
#endif
