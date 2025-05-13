#ifndef INFERENCE_DEFS_H
#define INFERENCE_DEFS_H

typedef enum {
  INFERENCE_TYPE_LEAVE,
  INFERENCE_TYPE_EXCHANGED,
  INFERENCE_TYPE_RACK,
} inference_stat_t;

typedef enum {
  INFERENCE_SUBTOTAL_DRAW,
  INFERENCE_SUBTOTAL_LEAVE,
} inference_subtotal_t;

#endif