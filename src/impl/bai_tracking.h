#ifndef BAI_TRACKING_H
#define BAI_TRACKING_H

#include "../ent/bai_logger.h"

typedef struct BAITracking BAITracking;

typedef enum { BAI_CTRACKING, BAI_DTRACKING } bai_tracking_t;

BAITracking *bai_tracking_create(bai_tracking_t type, const int *N,
                                 const int size);
void bai_tracking_destroy(BAITracking *bai_tracking);
int bai_tracking_track(const BAITracking *bai_tracking, const int *N,
                       const double *w, const int K, BAILogger *bai_logger);
void bai_tracking_swap_indexes(const BAITracking *bai_tracking, const int i,
                               const int j, BAILogger *bai_logger);
#endif