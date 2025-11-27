#include "endgame_results.h"

#include "../util/io_util.h"
#include <stdlib.h>

struct EndgameResults {
  PVLine pv_line;
  bool valid_for_current_game_state;
};

EndgameResults *endgame_results_create(void) {
  EndgameResults *endgame_results = malloc_or_die(sizeof(EndgameResults));
  endgame_results->valid_for_current_game_state = false;
  return endgame_results;
}

void endgame_results_destroy(EndgameResults *endgame_result) {
  free(endgame_result);
}

bool endgame_results_get_valid_for_current_game_state(
    const EndgameResults *endgame_result) {
  return endgame_result->valid_for_current_game_state;
}

void endgame_results_set_valid_for_current_game_state(
    EndgameResults *endgame_result, bool valid) {
  endgame_result->valid_for_current_game_state = valid;
}

const PVLine *endgame_results_get_pvline(const EndgameResults *endgame_result) {
  return &(endgame_result->pv_line);
}

void endgame_results_set_pvline(EndgameResults *endgame_result,
                                const PVLine *pv_line) {
  endgame_result->pv_line = *pv_line;
  endgame_results_set_valid_for_current_game_state(endgame_result, true);
}
