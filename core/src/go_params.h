#ifndef GO_PARAMS_H
#define GO_PARAMS_H

#include "rack.h"

typedef enum {
  SEARCH_TYPE_NONE,
  SEARCH_TYPE_SIM_MONTECARLO,
  SEARCH_TYPE_INFERENCE_SOLVE,
  SEARCH_TYPE_ENDGAME,
  SEARCH_TYPE_PREENDGAME,
  SEARCH_TYPE_STATICONLY,
} search_t;
typedef struct GoParams {
  search_t search_type;
  int depth;
  int stop_condition;
  int threads;
  int static_search_only;
  int num_plays;
  int max_iterations;
  char tiles[(RACK_SIZE) + 1];
  int player_index;
  int score;
  int number_of_tiles_exchanged;
  double equity_margin;
  int print_info_interval;
  int check_stopping_condition_interval;
} GoParams;

GoParams *create_go_params();
void destroy_go_params(GoParams *go_params);
void reset_go_params(GoParams *go_params);

#endif