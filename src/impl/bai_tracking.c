#include "bai_tracking.h"

#include <math.h>
#include <stdio.h>

#include "../ent/bai_logger.h"

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

int bai_c_track(const void *data, const int *N, const double *w, const int size,
                BAILogger *bai_logger) {
  CTracking *t = (CTracking *)data;
  for (int i = 0; i < size; i++) {
    t->sumw[i] += w[i];
  }
  int min_index = 0;
  for (int i = 1; i < size; i++) {
    if (N[i] - t->sumw[i] < N[min_index] - t->sumw[min_index]) {
      min_index = i;
    }
  }

  bai_logger_log_title(bai_logger, "C_TRACK");
  bai_logger_log_int_array(bai_logger, "N", N, size);
  bai_logger_log_double_array(bai_logger, "t.sumw", t->sumw, size);
  bai_logger_log_double_array(bai_logger, "w", w, size);
  bai_logger_log_int(bai_logger, "min_index", min_index);
  bai_logger_flush(bai_logger);

  return min_index;
}

int bai_d_track(const void __attribute__((unused)) * data, const int *N,
                const double *w, int size, BAILogger *bai_logger) {
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

  bai_logger_log_title(bai_logger, "D_TRACK");
  bai_logger_log_int_array(bai_logger, "N", N, size);
  bai_logger_log_double_array(bai_logger, "w", w, size);
  bai_logger_log_int(bai_logger, "argmin", argmin);
  bai_logger_flush(bai_logger);

  return argmin;
}

typedef int (*tracking_func_t)(const void *, const int *, const double *,
                               const int, BAILogger *);

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

int bai_track(BAITracking *bai_tracking, int *N, const double *w, const int K,
              BAILogger *bai_logger) {
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
    result = bai_tracking->tracking_func(
        bai_tracking->data, N, bai_tracking->undersampled, K, bai_logger);
  } else {
    result =
        bai_tracking->tracking_func(bai_tracking->data, N, w, K, bai_logger);
  }
  return result;
}
