#ifndef ENDGAME_RESULTS_H
#define ENDGAME_RESULTS_H

#include "../def/thread_control_defs.h"
#include "game.h"
#include "move.h"

// We don't expect an endgame length to ever be larger than this value.
enum { MAX_VARIANT_LENGTH = 25 };

typedef struct PVLine {
  SmallMove moves[MAX_VARIANT_LENGTH];
  Game *game;
  int32_t score;
  int num_moves;
} PVLine;

typedef struct EndgameResults EndgameResults;

EndgameResults *endgame_results_create(void);
void endgame_results_destroy(EndgameResults *endgame_result);
const PVLine *endgame_results_get_pvline(const EndgameResults *endgame_result);
void endgame_results_set_pvline(EndgameResults *endgame_result,
                                const PVLine *pv_line);

#endif
