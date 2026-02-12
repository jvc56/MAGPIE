#include "endgame_results.h"

#include "../util/io_util.h"
#include <stdlib.h>
#include <string.h>

struct EndgameResults {
  PVLine *pv_lines;
  int num_pv_lines;
  int capacity;
  bool valid_for_current_game_state;
};

EndgameResults *endgame_results_create(void) {
  EndgameResults *endgame_results = malloc_or_die(sizeof(EndgameResults));
  endgame_results->pv_lines = malloc_or_die(sizeof(PVLine));
  endgame_results->num_pv_lines = 0;
  endgame_results->capacity = 1;
  endgame_results->valid_for_current_game_state = false;
  return endgame_results;
}

void endgame_results_destroy(EndgameResults *endgame_result) {
  if (!endgame_result) {
    return;
  }
  free(endgame_result->pv_lines);
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
  return &endgame_result->pv_lines[0];
}

void endgame_results_set_pvline(EndgameResults *endgame_result,
                                const PVLine *pv_line) {
  endgame_result->pv_lines[0] = *pv_line;
  endgame_result->num_pv_lines = 1;
}

int endgame_results_get_num_pvlines(const EndgameResults *endgame_result) {
  return endgame_result->num_pv_lines;
}

const PVLine *endgame_results_get_pvline_at(
    const EndgameResults *endgame_result, int index) {
  return &endgame_result->pv_lines[index];
}

void endgame_results_set_pvlines(EndgameResults *endgame_result,
                                 const PVLine *pv_lines, int count) {
  if (count > endgame_result->capacity) {
    free(endgame_result->pv_lines);
    endgame_result->pv_lines = malloc_or_die(count * sizeof(PVLine));
    endgame_result->capacity = count;
  }
  memcpy(endgame_result->pv_lines, pv_lines, count * sizeof(PVLine));
  endgame_result->num_pv_lines = count;
}
