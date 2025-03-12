#include "bai_tracking.h"

#include <math.h>
#include <stdio.h>

#include "../util/util.h"

typedef struct CTracking {
  double *sumw;
} CTracking;

CTracking *create_c_tracking(const int *N, const int size) {
  CTracking *c_tracking = malloc_or_die(sizeof(CTracking));
  c_tracking->sumw = malloc_or_die(size * sizeof(double));
  for (int i = 0; i < size; i++) {
    c_tracking->sumw[i] = (double)N[i];
  }
  return c_tracking;
}

void destroy_c_tracking(CTracking *c_tracking) {
  free(c_tracking->sumw);
  free(c_tracking);
}

int bai_c_track(const void *data, const int *N, const double *w,
                const int size) {
  CTracking *t = (CTracking *)data;
  for (int i = 0; i < size; i++) {
    t->sumw[i] += w[i];
  }
  int argmin = 0;
  for (int i = 1; i < size; i++) {
    if (N[i] - t->sumw[i] < N[argmin] - t->sumw[argmin]) {
      argmin = i;
    }
  }
  return argmin;
}

int bai_d_track(const void __attribute__((unused)) * data, const int *N,
                const double *w, int size) {
  int sumN = 0;
  for (int i = 0; i < size; i++) {
    sumN += N[i];
  }
  int argmin = 0;
  for (int i = 1; i < size; i++) {
    if (N[i] - sumN * w[i] < N[argmin] - sumN * w[argmin]) {
      argmin = i;
    }
  }
  return argmin;
}

typedef int (*tracking_func_t)(const void *, const int *, const double *,
                               const int);

struct BAITracking {
  bai_tracking_t type;
  void *data;
  double *undersampled;
  tracking_func_t tracking_func;
};

BAITracking *bai_tracking_create(bai_tracking_t type, const int *N, int size) {
  BAITracking *bai_tracking = malloc_or_die(sizeof(BAITracking));
  bai_tracking->type = type;
  bai_tracking->data = NULL;
  bai_tracking->undersampled = malloc_or_die(size * sizeof(double));
  bai_tracking->tracking_func = NULL;
  switch (type) {
  case BAI_CTRACKING:
    bai_tracking->data = create_c_tracking(N, size);
    bai_tracking->tracking_func = bai_c_track;
    break;
  case BAI_DTRACKING:
    bai_tracking->tracking_func = bai_d_track;
    break;
  }
  return bai_tracking;
}

void bai_tracking_destroy(BAITracking *bai_tracking) {
  if (!bai_tracking) {
    return;
  }
  free(bai_tracking->undersampled);
  switch (bai_tracking->type) {
  case BAI_CTRACKING:
    destroy_c_tracking((CTracking *)bai_tracking->data);
    break;
  case BAI_DTRACKING:
    break;
  }
  free(bai_tracking);
}

int bai_track(BAITracking *bai_tracking, int *N, const double *w, const int K) {
  const int t = 0;
  for (int i = 0; i < K; i++) {
    N[i] += t;
  }
  int num_undersampled = 0;
  for (int i = 0; i < K; i++) {
    bai_tracking->undersampled[i] = 0.0;
    if (N[i] < sqrt((double)t) - (double)K / 2) {
      bai_tracking->undersampled[i] = 1.0;
      num_undersampled++;
    }
  }
  int result;
  if (num_undersampled > 0) {
    for (int i = 0; i < K; i++) {
      bai_tracking->undersampled[i] /= (double)num_undersampled;
    }
    result = bai_tracking->tracking_func(bai_tracking->data, N,
                                         bai_tracking->undersampled, K);
  } else {
    result = bai_tracking->tracking_func(bai_tracking->data, N, w, K);
  }
  return result;
}
