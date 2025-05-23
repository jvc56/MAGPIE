/*
 * Implements algorithms described in
 *
 * Dealing with Unknown Variances in Best-Arm Identification
 * (https://arxiv.org/pdf/2210.00974)
 *
 * with Julia source code kindly provided by Marc Jourdan.
 */
#ifndef BAI_TRACKING_H
#define BAI_TRACKING_H

#include "../def/bai_defs.h"

#include "bai_logger.h"

typedef struct BAITracking BAITracking;

BAITracking *bai_tracking_create(bai_tracking_t type, const int *N,
                                 const int size);
void bai_tracking_destroy(BAITracking *bai_tracking);
int bai_tracking_track(const BAITracking *bai_tracking, const int *N,
                       const double *w, const int K, BAILogger *bai_logger);
void bai_tracking_swap_indexes(const BAITracking *bai_tracking, const int i,
                               const int j, BAILogger *bai_logger);
#endif