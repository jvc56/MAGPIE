#include "endgame_results.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../util/io_util.h"
#include "game_history.h"
#include <stdlib.h>

typedef struct PVData {
  PVLine pv_line;
  int value;
  int depth;
  cpthread_mutex_t mutex;
} PVData;

struct EndgameResults {
  PVData best_pv_data;
  PVData display_pv_data;
  bool valid_for_current_game_state;
  Timer timer;
  double display_seconds_elapsed;
};

EndgameResults *endgame_results_create(void) {
  EndgameResults *endgame_results = malloc_or_die(sizeof(EndgameResults));
  endgame_results->best_pv_data.depth = -1;
  cpthread_mutex_init(&endgame_results->best_pv_data.mutex);
  cpthread_mutex_init(&endgame_results->display_pv_data.mutex);
  endgame_results->valid_for_current_game_state = false;
  ctimer_reset(&endgame_results->timer);
  return endgame_results;
}

void endgame_results_destroy(EndgameResults *endgame_results) {
  free(endgame_results);
}

// NOT THREAD SAFE: Caller must ensure synchronization
void endgame_results_reset(EndgameResults *endgame_results) {
  endgame_results->best_pv_data.depth = -1;
  endgame_results->valid_for_current_game_state = false;
  ctimer_start(&endgame_results->timer);
}

bool endgame_results_get_valid_for_current_game_state(
    const EndgameResults *endgame_results) {
  return endgame_results->valid_for_current_game_state;
}

void endgame_results_set_valid_for_current_game_state(
    EndgameResults *endgame_results, bool valid) {
  endgame_results->valid_for_current_game_state = valid;
}

// NOT THREAD SAFE: Caller must ensure synchronization
const PVLine *endgame_results_get_pvline(const EndgameResults *endgame_results,
                                         endgame_result_t result_type) {
  const PVLine *pv_line = NULL;
  switch (result_type) {
  case ENDGAME_RESULT_BEST:
    pv_line = &endgame_results->best_pv_data.pv_line;
    break;
  case ENDGAME_RESULT_DISPLAY:
    pv_line = &endgame_results->display_pv_data.pv_line;
    break;
  }
  return pv_line;
}

// NOT THREAD SAFE: Caller must ensure synchronization
int endgame_results_get_value(const EndgameResults *endgame_results,
                              endgame_result_t result_type) {
  int value;
  switch (result_type) {
  case ENDGAME_RESULT_BEST:
    value = endgame_results->best_pv_data.value;
    break;
  case ENDGAME_RESULT_DISPLAY:
    value = endgame_results->display_pv_data.value;
    break;
  }
  return value;
}

// NOT THREAD SAFE: Caller must ensure synchronization
int endgame_results_get_depth(const EndgameResults *endgame_results,
                              endgame_result_t result_type) {
  int depth;
  switch (result_type) {
  case ENDGAME_RESULT_BEST:
    depth = endgame_results->best_pv_data.depth;
    break;
  case ENDGAME_RESULT_DISPLAY:
    depth = endgame_results->display_pv_data.depth;
    break;
  }
  return depth;
}

double endgame_results_get_display_seconds_elapsed(
    const EndgameResults *endgame_results) {
  return endgame_results->display_seconds_elapsed;
}

void endgame_results_lock(EndgameResults *endgame_results,
                          endgame_result_t result_type) {
  switch (result_type) {
  case ENDGAME_RESULT_BEST:
    cpthread_mutex_lock(&endgame_results->best_pv_data.mutex);
    break;
  case ENDGAME_RESULT_DISPLAY:
    cpthread_mutex_lock(&endgame_results->display_pv_data.mutex);
    break;
  }
}

void endgame_results_unlock(EndgameResults *endgame_results,
                            endgame_result_t result_type) {
  switch (result_type) {
  case ENDGAME_RESULT_BEST:
    cpthread_mutex_unlock(&endgame_results->best_pv_data.mutex);
    break;
  case ENDGAME_RESULT_DISPLAY:
    cpthread_mutex_unlock(&endgame_results->display_pv_data.mutex);
    break;
  }
}

// NOT THREAD SAFE: Caller must ensure synchronization
void endgame_results_update_display_data(EndgameResults *endgame_results) {
  endgame_results->display_pv_data = endgame_results->best_pv_data;
  endgame_results->display_seconds_elapsed =
      ctimer_elapsed_seconds(&endgame_results->timer);
}

void endgame_results_set_best_pvline(EndgameResults *endgame_results,
                                     const PVLine *pv_line, int value,
                                     int depth) {
  endgame_results_lock(endgame_results, ENDGAME_RESULT_BEST);
  if (depth > endgame_results->best_pv_data.depth) {
    endgame_results->best_pv_data.depth = depth;
    endgame_results->best_pv_data.value = value;
    endgame_results->best_pv_data.pv_line = *pv_line;
  }
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_BEST);
}