#ifndef SIM_PARAMS_H
#define SIM_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SimParams {
  int plies;
  int num_plays;
  int max_iterations;
  int min_play_iterations;
  double stop_cond_pct;
  bool use_inference;
} SimParams;

#endif
