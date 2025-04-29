#include "bai_helper.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>

#include "../def/bai_defs.h"

#include "bai_logger.h"

#include "../util/log.h"
#include "../util/math_util.h"
#include "../util/util.h"

#include "../def/bai_defs.h"
#include "../def/math_util_defs.h"

typedef struct GK16 {
  double δ;
} GK16;

void *create_GK16(const double δ) {
  GK16 *gk16 = malloc_or_die(sizeof(GK16));
  gk16->δ = δ;
  return gk16;
}

void destroy_GK16(GK16 *gk16) { free(gk16); }

double GK16_threshold(const void *data, const int *N, const int K,
                      const double __attribute__((unused)) * hμ,
                      const double __attribute__((unused)) * hσ2,
                      const int __attribute__((unused)) astar,
                      const int __attribute__((unused)) a,
                      BAILogger *bai_logger) {
  const GK16 *gk16 = (GK16 *)data;
  int t = 0;
  for (int i = 0; i < K; i++) {
    t += N[i];
  }
  const double result = log((log((double)t) + 1) / gk16->δ);
  bai_logger_log_title(bai_logger, "GK16");
  bai_logger_log_int(bai_logger, "t", t);
  bai_logger_log_double(bai_logger, "result", result);
  bai_logger_flush(bai_logger);
  return result;
}

typedef double (*threshold_func_t)(const void *, const int *, const int,
                                   const double *, const double *, const int,
                                   const int, BAILogger *);

struct BAIThreshold {
  bai_threshold_t type;
  void *data;
  threshold_func_t threshold_func;
};

BAIThreshold *bai_create_threshold(const bai_threshold_t type, const double δ,
                                   const int __attribute__((unused)) r,
                                   const int __attribute__((unused)) s,
                                   const double __attribute__((unused)) γ) {
  BAIThreshold *bai_threshold = NULL;
  switch (type) {
  case BAI_THRESHOLD_GK16:
    bai_threshold = malloc_or_die(sizeof(BAIThreshold));
    bai_threshold->type = type;
    bai_threshold->data = create_GK16(δ);
    bai_threshold->threshold_func = GK16_threshold;
    break;
  case BAI_THRESHOLD_NONE:
    break;
  }
  return bai_threshold;
}

void bai_destroy_threshold(BAIThreshold *bai_threshold) {
  if (!bai_threshold) {
    return;
  }
  switch (bai_threshold->type) {
  case BAI_THRESHOLD_GK16:
    destroy_GK16((GK16 *)bai_threshold->data);
    break;
  case BAI_THRESHOLD_NONE:
    break;
  }
  free(bai_threshold);
}

double bai_invoke_threshold(const BAIThreshold *bai_threshold, const int *N,
                            const int K, const double *hμ, const double *hσ2,
                            const int astar, const int a,
                            BAILogger *bai_logger) {
  return bai_threshold->threshold_func(bai_threshold->data, N, K, hμ, hσ2,
                                       astar, a, bai_logger);
}