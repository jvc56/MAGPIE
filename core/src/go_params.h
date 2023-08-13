#ifndef GO_PARAMS_H
#define GO_PARAMS_H

#define SEARCH_TYPE_NONE 0
#define SEARCH_TYPE_SIM_MONTECARLO 1
#define SEARCH_TYPE_INFERENCE_SOLVE 2
#define SEARCH_TYPE_ENDGAME 3
#define SEARCH_TYPE_PREENDGAME 4
#define SEARCH_TYPE_STATICONLY 5

typedef struct GoParams {
  int search_type;
  int depth;
  int stop_condition;
  int threads;
  int static_search_only;
  int num_plays;
  int max_iterations;
  int print_info_interval;
  int check_stopping_condition_interval;
} GoParams;

GoParams *create_go_params();
void destroy_go_params(GoParams *go_params);
void reset_go_params(GoParams *go_params);

#endif