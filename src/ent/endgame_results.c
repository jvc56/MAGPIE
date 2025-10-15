#include "endgame_results.h"
#include "../util/io_util.h"

struct EndgameResults {
  PVLine pv_line;
};

EndgameResults *endgame_results_create(void) {
  return malloc_or_die(sizeof(EndgameResults));
}

void endgame_results_destroy(EndgameResults *endgame_result) {
  free(endgame_result);
}

const PVLine *endgame_results_get_pvline(const EndgameResults *endgame_result) {
  return &(endgame_result->pv_line);
}

void endgame_results_set_pvline(EndgameResults *endgame_result,
                                const PVLine *pv_line) {
  endgame_result->pv_line = *pv_line;
}