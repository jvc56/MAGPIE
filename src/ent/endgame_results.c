#include "endgame_results.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../ent/equity.h"
#include "../ent/player.h"
#include "../ent/transposition_table.h"
#include "../util/io_util.h"
#include "game.h"
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
  double seconds_elapsed;
  // Snapshot of the game state when the endgame solve began; used by
  // shendgame to decode the PV even if config->game has since changed (e.g.
  // after newgame).
  Game *start_game;
  // Top-K PVs from the most recent completed solve.  The array is grown with
  // realloc_or_die as needed and freed only on destroy (reset keeps it).
  PVLine *multi_pvs;
  _Atomic int num_pvs;
  int multi_pvs_capacity;
  // Arguments forwarded to pvline_extend_from_tt for PV display extension.
  TranspositionTable *tt;
  int solving_player;
  int max_depth;
  // Reusable game copy for PV extension (avoid repeated game_duplicate).
  Game *ext_game;
  endgame_result_status_t status;
};

EndgameResults *endgame_results_create(void) {
  EndgameResults *endgame_results = malloc_or_die(sizeof(EndgameResults));
  endgame_results->best_pv_data.depth = -1;
  endgame_results->best_pv_data.pv_line.num_moves = 0;
  endgame_results->display_pv_data.depth = -1;
  endgame_results->display_pv_data.value = 0;
  endgame_results->display_pv_data.pv_line.num_moves = 0;
  cpthread_mutex_init(&endgame_results->best_pv_data.mutex);
  cpthread_mutex_init(&endgame_results->display_pv_data.mutex);
  endgame_results->valid_for_current_game_state = false;
  ctimer_reset(&endgame_results->timer);
  endgame_results->seconds_elapsed = 0;
  endgame_results->start_game = NULL;
  endgame_results->multi_pvs = NULL;
  endgame_results->num_pvs = 0;
  endgame_results->multi_pvs_capacity = 0;
  endgame_results->tt = NULL;
  endgame_results->solving_player = 0;
  endgame_results->max_depth = 0;
  endgame_results->ext_game = NULL;
  endgame_results->status = ENDGAME_RESULT_STATUS_NONE;
  return endgame_results;
}

void endgame_results_destroy(EndgameResults *endgame_results) {
  if (!endgame_results) {
    return;
  }
  game_destroy(endgame_results->start_game);
  game_destroy(endgame_results->ext_game);
  free(endgame_results->multi_pvs);
  free(endgame_results);
}

// NOT THREAD SAFE: Caller must ensure synchronization
void endgame_results_reset(EndgameResults *endgame_results) {
  endgame_results->best_pv_data.depth = -1;
  endgame_results->best_pv_data.pv_line.num_moves = 0;
  endgame_results->display_pv_data.depth = -1;
  endgame_results->display_pv_data.pv_line.num_moves = 0;
  endgame_results->valid_for_current_game_state = false;
  ctimer_start(&endgame_results->timer);
  game_destroy(endgame_results->start_game);
  endgame_results->start_game = NULL;
  // Keep multi_pvs buffer alive for reuse; just reset the count.
  endgame_results->num_pvs = 0;
  endgame_results->tt = NULL;
  endgame_results->solving_player = 0;
  endgame_results->max_depth = 0;
  endgame_results->status = ENDGAME_RESULT_STATUS_NONE;
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

int endgame_results_get_spread(const EndgameResults *endgame_results,
                               endgame_result_t result_type, const Game *game) {
  int value;
  switch (result_type) {
  case ENDGAME_RESULT_BEST:
    value = endgame_results->best_pv_data.value;
    break;
  case ENDGAME_RESULT_DISPLAY:
    value = endgame_results->display_pv_data.value;
    break;
  }
  const int p0_score =
      equity_to_int(player_get_score(game_get_player(game, 0)));
  const int p1_score =
      equity_to_int(player_get_score(game_get_player(game, 1)));
  const int initial_on_turn_spread = (endgame_results->solving_player == 0)
                                         ? p0_score - p1_score
                                         : p1_score - p0_score;
  return initial_on_turn_spread + value;
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

double
endgame_results_get_seconds_elapsed(const EndgameResults *endgame_results) {
  return endgame_results->seconds_elapsed;
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

void endgame_results_update_display_data(EndgameResults *endgame_results) {
  endgame_results->display_pv_data.pv_line =
      endgame_results->best_pv_data.pv_line;
  endgame_results->display_pv_data.value = endgame_results->best_pv_data.value;
  endgame_results->display_pv_data.depth = endgame_results->best_pv_data.depth;
  endgame_results->seconds_elapsed =
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

void endgame_results_set_start_game(EndgameResults *endgame_results,
                                    const Game *game) {
  game_destroy(endgame_results->start_game);
  endgame_results->start_game = game_duplicate(game);
}

const Game *
endgame_results_get_start_game(const EndgameResults *endgame_results) {
  return endgame_results->start_game;
}

void endgame_results_stop_ctimer(EndgameResults *endgame_results) {
  ctimer_stop(&endgame_results->timer);
}

// NOT THREAD SAFE: Caller must ensure synchronization
void endgame_results_ensure_pvs_capacity(EndgameResults *endgame_results,
                                         int n) {
  if (n > endgame_results->multi_pvs_capacity) {
    endgame_results->multi_pvs =
        realloc_or_die(endgame_results->multi_pvs, n * sizeof(PVLine));
    endgame_results->multi_pvs_capacity = n;
  }
}

PVLine *endgame_results_get_multi_pvs(const EndgameResults *endgame_results) {
  return endgame_results->multi_pvs;
}

void endgame_results_set_num_pvs(EndgameResults *endgame_results, int num_pvs) {
  atomic_store(&endgame_results->num_pvs, num_pvs);
}

int endgame_results_get_num_pvs(const EndgameResults *endgame_results) {
  return atomic_load(&endgame_results->num_pvs);
}

const PVLine *
endgame_results_get_multi_pvline(const EndgameResults *endgame_results,
                                 int idx) {
  return &endgame_results->multi_pvs[idx];
}

void endgame_results_set_pvline_extend_args(EndgameResults *endgame_results,
                                            TranspositionTable *tt,
                                            int solving_player, int max_depth) {
  endgame_results->tt = tt;
  endgame_results->solving_player = solving_player;
  endgame_results->max_depth = max_depth;
}

TranspositionTable *
endgame_results_get_tt(const EndgameResults *endgame_results) {
  return endgame_results->tt;
}

int endgame_results_get_solving_player(const EndgameResults *endgame_results) {
  return endgame_results->solving_player;
}

int endgame_results_get_max_depth(const EndgameResults *endgame_results) {
  return endgame_results->max_depth;
}

endgame_result_status_t
endgame_results_get_status(const EndgameResults *endgame_results) {
  return endgame_results->status;
}

void endgame_results_set_status(EndgameResults *endgame_results,
                                endgame_result_status_t status) {
  endgame_results->status = status;
}

Game *endgame_results_prepare_ext_game(EndgameResults *endgame_results,
                                       const Game *source_game) {
  if (!endgame_results->ext_game) {
    endgame_results->ext_game = game_duplicate(source_game);
  } else {
    game_copy(endgame_results->ext_game, source_game);
  }
  game_set_endgame_solving_mode(endgame_results->ext_game);
  return endgame_results->ext_game;
}
