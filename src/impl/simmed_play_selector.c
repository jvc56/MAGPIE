#include "simmed_play_selector.h"

#include "../ent/equity.h"
#include "../ent/sim_results.h"

// These variable names follow the TrackAndStop2 algorithm
// implemented in
// https://github.com/jsfunc/best-arm-identification/blob/master/BAIalgos.jl
typedef struct SPSTrackAndStop {
  bool condition;
  // The number of simmed plays
  int K;
  // The number of samples for each simmed play
  uint64_t *N;
  // The sum of equity for each simmed play
  double *S;
  // The weights for each simmed play sum
  double *SumWeights;
  // The average equity for each simmed play
  double *Mu;
  // Mu incremented by the best option
  double *MuMid;
} SPSTrackAndStop;

typedef struct TASOptimalWeightsOutput {
  double vOpt;
  double NuOpt;
} TASOptimalWeightsOutput;

SPS *sps_tas_create(int num_simmed_plays) {
  SPSTrackAndStop *tas = malloc(sizeof(SPSTrackAndStop));
  tas->condition = true;
  return (SPS *)tas;
}

void sps_tas_destroy(SPS *tas) {
  if (!tas) {
    return;
  }
  free(tas);
}

int find_index_of_max_mu(double *mu, int K) {
  int index = 0;
  double max_mu = mu[0];
  for (int i = 1; i < K; i++) {
    if (mu[i] > max_mu) {
      max_mu = mu[i];
      index = i;
    }
  }
  return index;
}

void OptimalWeights(double *mu, int K, double delta) {
  int IndMax = find_index_of_max_mu(mu, K);
}

int sps_tas_select(SPS *tas) {}

void sps_tas_add_equity(SPS *tas, Equity equity, int index) {}

typedef struct SPSUniform {
} SPSUniform;

SPS *sps_uni_create() {}

void sps_uni_destroy(SPS *uni) {}

int sps_uni_select(SPS *uni) {}

void sps_uni_add_equity(SPS *uni, Equity equity, int index) {}

typedef int (*sps_select_func_t)(SPS *);
typedef void (*sps_add_equity_func_t)(SPS *, Equity, int);

struct SPS {
  void *state;
  sps_select_func_t select_func;
  sps_add_equity_func_t add_equity_func;
  sps_t type;
};

SPS *sps_create(sps_t sps_type, int num_simmed_plays) {
  SPS *sps = NULL;
  switch (sps_type) {
  case SPS_UNIFORM:
    sps = sps_uni_create();
    break;
  case SPS_TRACK_AND_STOP:
    sps = sps_tas_create(num_simmed_plays);
    break;
  }
  sps->type = sps_type;
  return sps;
}

void sps_destroy(SPS *sps) {
  switch (sps->type) {
  case SPS_UNIFORM:
    sps_uni_destroy(sps);
    break;
  case SPS_TRACK_AND_STOP:
    sps_tas_destroy(sps);
    break;
  }
}

// Returns -1 if the selector is finished.
int sps_select(SPS *sps) { sps->select_func(sps); }

void sps_add_equity(SPS *sps, Equity equity, int index) {
  sps->add_equity_func(sps, equity, index);
}