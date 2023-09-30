#include <stdlib.h>

#include "constants.h"
#include "go_params.h"
#include "util.h"

void reset_go_params(GoParams *go_params) {
  go_params->search_type = SEARCH_TYPE_NONE;
  go_params->depth = 0;
  go_params->stop_condition = SIM_STOPPING_CONDITION_NONE;
  go_params->threads = 0;
  go_params->static_search_only = 0;
  go_params->num_plays = 0;
  go_params->max_iterations = 0;
  go_params->tiles[0] = '\0';
  go_params->player_index = 0;
  go_params->score = 0;
  go_params->number_of_tiles_exchanged = 0;
  go_params->equity_margin = 0;
  go_params->print_info_interval = 0;
  go_params->check_stopping_condition_interval = 0;
}

GoParams *create_go_params() {
  GoParams *go_params = malloc_or_die(sizeof(GoParams));
  reset_go_params(go_params);
  return go_params;
}

void destroy_go_params(GoParams *go_params) { free(go_params); }
